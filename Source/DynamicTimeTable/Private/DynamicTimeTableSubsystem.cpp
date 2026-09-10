#include "DynamicTimeTableSubsystem.h"

#include "DTTSaveDataActor.h"
#include "DynamicTimeTableChatCommand.h"
#include "Command/ChatCommandLibrary.h"
#include "FGTrain.h"
#include "FGRailroadTimeTable.h"
#include "FGTrainStationIdentifier.h"
#include "FGRailroadSubsystem.h"
#include "Buildables/FGBuildableRailroadStation.h"
#include "WheeledVehicles/FGVehicleSubsystem.h"
#include "WheeledVehicles/FGDockingStationIdentifier.h"
#include "WheeledVehicles/FGWheeledVehicleIdentifier.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "TimerManager.h"
#include "Misc/SecureHash.h"

DEFINE_LOG_CATEGORY_STATIC(LogDynamicTimeTableSubsystem, Log, All);

namespace
{
    FString DTSStationName(const AFGTrainStationIdentifier* Identifier)
    {
        return IsValid(Identifier) ? Identifier->GetStationName().ToString() : TEXT("<none>");
    }

    FString DTSDockName(const AFGDockingStationIdentifier* Identifier)
    {
        return IsValid(Identifier) ? Identifier->GetStationName().ToString() : TEXT("<none>");
    }

    FString DTSVehicleName(const AFGWheeledVehicleIdentifier* Vehicle)
    {
        return IsValid(Vehicle) ? Vehicle->GetVehicleName().ToString() : TEXT("<none>");
    }

    bool DTSIsAllDigits(const FString& Text)
    {
        if (Text.IsEmpty()) return false;
        for (const TCHAR Ch : Text) if (Ch < TEXT('0') || Ch > TEXT('9')) return false;
        return true;
    }

    /**
     * A group has no stored identity any more - it is just a name on a station - so its id has to
     * be derived from that name. It has to stay the same across rebuilds, or the rotation cursor
     * and any open client window would lose their group every time a station is renamed.
     */
    FGuid DTSMakeGroupId(EDTTGroupKind Kind, const FString& GroupName)
    {
        const FString Key = FString::Printf(TEXT("%d|%s"), static_cast<int32>(Kind), *GroupName.ToLower());
        const auto Utf8 = StringCast<UTF8CHAR>(*Key);
        FMD5 Md5;
        Md5.Update(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
        uint8 Digest[16];
        Md5.Final(Digest);
        uint32 Parts[4];
        FMemory::Memcpy(Parts, Digest, sizeof(Parts));
        return FGuid(Parts[0], Parts[1], Parts[2], Parts[3]);
    }
}

bool UDynamicTimeTableSubsystem::ParseStationTag(const FString& FullName, FString& OutBaseName, FString& OutGroupName, int32& OutTargetSlots)
{
    OutBaseName = FullName.TrimStartAndEnd();
    OutGroupName.Reset();
    OutTargetSlots = 1;

    int32 Close = INDEX_NONE;
    if (!FullName.FindLastChar(TEXT(']'), Close)) return false;
    int32 Open = INDEX_NONE;
    if (!FullName.Left(Close).FindLastChar(TEXT('['), Open)) return false;

    FString Inner = FullName.Mid(Open + 1, Close - Open - 1).TrimStartAndEnd();
    if (Inner.IsEmpty()) return false;

    // The slot count is whatever follows the last comma, and only if it is a plain number - so a
    // group genuinely called "Depot, North" keeps its comma instead of losing half its name.
    int32 Comma = INDEX_NONE;
    if (Inner.FindLastChar(TEXT(','), Comma))
    {
        const FString Tail = Inner.Mid(Comma + 1).TrimStartAndEnd();
        if (DTSIsAllDigits(Tail))
        {
            const int32 Parsed = FCString::Atoi(*Tail);
            const FString Head = Inner.Left(Comma).TrimStartAndEnd();
            if (Parsed >= 1 && !Head.IsEmpty())
            {
                OutTargetSlots = Parsed;
                Inner = Head;
            }
        }
    }

    if (Inner.IsEmpty()) return false;
    OutGroupName = Inner;
    OutBaseName = (FullName.Left(Open) + FullName.Mid(Close + 1)).TrimStartAndEnd();
    return true;
}

FString UDynamicTimeTableSubsystem::StripStationTag(const FString& FullName)
{
    FString Base, Group; int32 Slots = 1;
    ParseStationTag(FullName, Base, Group, Slots);
    return Base;
}

FString UDynamicTimeTableSubsystem::SanitizeGroupName(const FString& GroupName)
{
    FString Clean = GroupName;
    Clean.ReplaceInline(TEXT("["), TEXT(" "));
    Clean.ReplaceInline(TEXT("]"), TEXT(" "));
    Clean.ReplaceInline(TEXT(","), TEXT(" "));
    return Clean.TrimStartAndEnd();
}

FString UDynamicTimeTableSubsystem::MakeTaggedName(const FString& BaseName, const FString& GroupName, int32 TargetSlots)
{
    const FString Base = StripStationTag(BaseName);
    const FString Group = SanitizeGroupName(GroupName);
    if (Group.IsEmpty()) return Base;
    const int32 Slots = FMath::Max(1, TargetSlots);
    const FString Tag = Slots > 1 ? FString::Printf(TEXT("[%s,%d]"), *Group, Slots)
                                  : FString::Printf(TEXT("[%s]"), *Group);
    return Base.IsEmpty() ? Tag : FString::Printf(TEXT("%s %s"), *Base, *Tag);
}

void UDynamicTimeTableSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    UE_LOG(LogDynamicTimeTableSubsystem, Log, TEXT("DynamicTimeTable subsystem initialized."));
}

void UDynamicTimeTableSubsystem::Deinitialize()
{
    ActiveBookings.Reset();
    TruckBookings.Reset();
    InternalTimeTableChanges.Reset();
    InternalRouteChanges.Reset();
    PendingTimeTableReevaluations.Reset();
    PendingRouteReevaluations.Reset();
    bTimeTableReevaluationScheduled = false;
    bRouteReevaluationScheduled = false;
    GroupRotationCursor.Reset();
    TruckPollStates.Reset();
    DraftGroups.Reset();
    InternalNameChanges.Reset();
    bStationFingerprintValid = false;
    MigrationAttempts = 0;
    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(TruckPollTimerHandle);
    }
    bWorldStationsReady = false;
    StationGroups.Reset();
    SaveDataActor.Reset();
    bPersistenceReady = false;
    Super::Deinitialize();
}

bool UDynamicTimeTableSubsystem::IsRuntimeGameWorld() const
{
    const UWorld* World = GetWorld();
    return IsValid(World) && !IsRunningCommandlet() &&
        (World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE);
}

bool UDynamicTimeTableSubsystem::IsServer() const
{
    const UWorld* World = GetWorld();
    return IsValid(World) && World->GetNetMode() != NM_Client;
}

void UDynamicTimeTableSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
    Super::OnWorldBeginPlay(InWorld);
    if (IsRuntimeGameWorld())
    {
        InitializePersistence();
        RegisterDTTChatCommand();
    }
}

void UDynamicTimeTableSubsystem::RegisterDTTChatCommand()
{
    AChatCommandSubsystem* ChatSubsystem = AChatCommandSubsystem::Get(this);
    if (!IsValid(ChatSubsystem))
    {
        UE_LOG(LogDynamicTimeTableSubsystem, Warning, TEXT("dTT chat command registration skipped: chat subsystem unavailable."));
        return;
    }

    ChatSubsystem->RegisterCommand(
        TEXT("DynamicTimeTable"),
        ADynamicTimeTableChatCommand::StaticClass());

    UE_LOG(LogDynamicTimeTableSubsystem, Display, TEXT("Registered chat command: /dtt"));
}

void UDynamicTimeTableSubsystem::InitializePersistence()
{
    if (bPersistenceReady || !IsRuntimeGameWorld()) return;

    ADTTSaveDataActor* Found = nullptr;
    for (TActorIterator<ADTTSaveDataActor> It(GetWorld()); It; ++It)
    {
        Found = *It;
        break;
    }

    if (IsValid(Found))
    {
        SaveDataActor = Found;
        // Deliberately not copied into StationGroups. Groups are derived from station names now;
        // what the actor holds is only a legacy configuration waiting to be migrated into tags.
        UE_LOG(LogDynamicTimeTableSubsystem, Display,
            TEXT("Found dTT save actor. LegacyGroups=%d DataVersion=%d"), Found->GetGroups().Num(), Found->GetDataVersion());
    }
    else if (GetWorld()->GetNetMode() != NM_Client)
    {
        FActorSpawnParameters Params;
        Params.Name = TEXT("DTT_SaveData");
        Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        Found = GetWorld()->SpawnActor<ADTTSaveDataActor>(
            ADTTSaveDataActor::StaticClass(), FTransform::Identity, Params);
        SaveDataActor = Found;

        UE_LOG(LogDynamicTimeTableSubsystem, Display,
            TEXT("Created new DynamicTimeTable save-data actor."));
    }

    bPersistenceReady = true;
    StationGroups.Reset();
    // Nothing is read from the world yet: at OnWorldBeginPlay the railroad and vehicle subsystems
    // have not registered their identifiers, so every station lookup would come back empty.
    ScheduleWorldStationsReady();
    StartTruckPolling();
    OnGroupsChanged.Broadcast(); OnGroupsChangedNative.Broadcast();
}

void UDynamicTimeTableSubsystem::SyncConfigurationToSaveActor()
{
    // The save actor no longer stores anything: the configuration lives in the station names,
    // which the game saves itself. It is kept, empty, because the class is referenced by existing
    // saves and pulling a class out from under a load is worse than an idle actor.
    if (SaveDataActor.IsValid() && IsServer())
    {
        SaveDataActor->SetGroups(TArray<FDTTStationGroup>());
        SaveDataActor->SetDataVersion(4);
    }
}

// ---------------------------------------------------------------------------
// Editing a group means editing a station name. Every entry point below ends in
// a rename plus a rebuild, so the window and the vanilla station panel are two
// ways of doing exactly the same thing and cannot disagree.
// ---------------------------------------------------------------------------

const FDTTStationGroup* UDynamicTimeTableSubsystem::FindGroupOrDraft(FGuid GroupId) const
{
    if (const FDTTStationGroup* Group = FindGroup(GroupId)) return Group;
    return DraftGroups.Find(GroupId);
}

bool UDynamicTimeTableSubsystem::RenameStationGuarded(AFGTrainStationIdentifier* Identifier, const FString& NewName)
{
    if (!IsValid(Identifier) || !IsServer()) return false;
    if (Identifier->GetStationName().ToString() == NewName) return true;
    // The rename hook has to be able to tell this apart from a player edit, or it would rebuild
    // from the middle of the loop that is still renaming.
    InternalNameChanges.Add(Identifier);
    Identifier->SetStationName(FText::FromString(NewName));
    InternalNameChanges.Remove(Identifier);
    const bool bOk = Identifier->GetStationName().ToString() == NewName;
    UE_LOG(LogDynamicTimeTableSubsystem, Display, TEXT("STATION_RETAGGED Kind=Train New='%s' Applied=%s"),
        *NewName, bOk ? TEXT("true") : TEXT("false"));
    return bOk;
}

bool UDynamicTimeTableSubsystem::RenameDockGuarded(AFGDockingStationIdentifier* Identifier, const FString& NewName)
{
    if (!IsValid(Identifier) || !IsServer()) return false;
    if (Identifier->GetStationName().ToString() == NewName) return true;
    InternalNameChanges.Add(Identifier);
    Identifier->SetStationName(FText::FromString(NewName));
    InternalNameChanges.Remove(Identifier);
    const bool bOk = Identifier->GetStationName().ToString() == NewName;
    UE_LOG(LogDynamicTimeTableSubsystem, Display, TEXT("STATION_RETAGGED Kind=Truck New='%s' Applied=%s"),
        *NewName, bOk ? TEXT("true") : TEXT("false"));
    return bOk;
}

bool UDynamicTimeTableSubsystem::RetagStation(const FDTTStationGroup& Group, const FDTTStationEntry& Entry, const FString& NewGroupName, int32 NewTargetSlots)
{
    const FString NewName = MakeTaggedName(Entry.DisplayName, NewGroupName, NewTargetSlots);
    if (Group.Kind == EDTTGroupKind::Truck)
    {
        AFGDockingStationIdentifier* Dock = Entry.DockingStationIdentifier.Get();
        if (!IsValid(Dock)) Dock = FindDockByName(Entry.DisplayName);
        return RenameDockGuarded(Dock, NewName);
    }
    AFGTrainStationIdentifier* Station = Entry.StationIdentifier.Get();
    if (!IsValid(Station)) Station = FindVanillaStationByName(Entry.DisplayName);
    return RenameStationGuarded(Station, NewName);
}

FGuid UDynamicTimeTableSubsystem::CreateGroup(const FString& GroupName, EDTTGroupKind Kind)
{
    FString Name = SanitizeGroupName(GroupName);
    if (Name.IsEmpty()) Name = TEXT("New Station Group");
    const FGuid GroupId = DTSMakeGroupId(Kind, Name);

    // Creating a group writes nothing anywhere: a group exists exactly as long as some station
    // carries its tag. Until the first station is added it is only a draft, kept for this session
    // so the window has something to show.
    if (!FindGroup(GroupId) && !DraftGroups.Contains(GroupId))
    {
        FDTTStationGroup Draft;
        Draft.GroupId = GroupId;
        Draft.GroupName = Name;
        Draft.Kind = Kind;
        DraftGroups.Add(GroupId, Draft);
        ConfigurationChanged(false);
    }

    UE_LOG(LogDynamicTimeTableSubsystem, Display, TEXT("Created group '%s' Kind=%s GUID=%s"), *Name,
        Kind == EDTTGroupKind::Truck ? TEXT("Truck") : TEXT("Train"), *GroupId.ToString());
    return GroupId;
}

bool UDynamicTimeTableSubsystem::RemoveGroup(FGuid GroupId)
{
    const bool bWasDraft = DraftGroups.Remove(GroupId) > 0;
    const FDTTStationGroup* Group = FindGroup(GroupId);
    if (!Group)
    {
        if (bWasDraft) ConfigurationChanged(false);
        return bWasDraft;
    }

    const FDTTStationGroup Snapshot = *Group;
    for (const FDTTStationEntry& Entry : Snapshot.Stations)
    {
        RetagStation(Snapshot, Entry, FString(), 1);
    }
    GroupRotationCursor.Remove(GroupId);
    RebuildGroupsFromNames();
    return true;
}

bool UDynamicTimeTableSubsystem::RenameGroup(FGuid GroupId, const FString& NewName)
{
    const FString Name = SanitizeGroupName(NewName);
    if (Name.IsEmpty()) return false;

    if (FDTTStationGroup* Draft = DraftGroups.Find(GroupId))
    {
        FDTTStationGroup Renamed = *Draft;
        Renamed.GroupName = Name;
        Renamed.GroupId = DTSMakeGroupId(Renamed.Kind, Name);
        DraftGroups.Remove(GroupId);
        DraftGroups.Add(Renamed.GroupId, Renamed);
        ConfigurationChanged(false);
        return true;
    }

    const FDTTStationGroup* Group = FindGroup(GroupId);
    if (!Group) return false;

    const FDTTStationGroup Snapshot = *Group;
    bool bAny = false;
    for (const FDTTStationEntry& Entry : Snapshot.Stations)
    {
        bAny |= RetagStation(Snapshot, Entry, Name, Entry.TargetSlots);
    }
    if (!bAny) return false;
    GroupRotationCursor.Remove(GroupId);
    RebuildGroupsFromNames();
    return true;
}

bool UDynamicTimeTableSubsystem::AddStationByName(FGuid GroupId, const FString& StationName, int32 TargetSlots)
{
    const FDTTStationGroup* Group = FindGroupOrDraft(GroupId);
    const FString Name = StationName.TrimStartAndEnd();
    if (!Group || Name.IsEmpty()) return false;

    const FString GroupName = Group->GroupName;
    const int32 Slots = FMath::Max(1, TargetSlots);
    const FString NewName = MakeTaggedName(Name, GroupName, Slots);

    bool bOk = false;
    if (Group->Kind == EDTTGroupKind::Truck)
    {
        bOk = RenameDockGuarded(FindDockByName(Name), NewName);
    }
    else
    {
        bOk = RenameStationGuarded(FindVanillaStationByName(Name), NewName);
    }
    if (!bOk) return false;

    DraftGroups.Remove(GroupId);
    RebuildGroupsFromNames();
    return true;
}

bool UDynamicTimeTableSubsystem::RemoveStationByName(FGuid GroupId, const FString& StationName)
{
    const FDTTStationGroup* Group = FindGroup(GroupId);
    const FDTTStationEntry* Entry = Group ? FindStationEntry(*Group, StationName) : nullptr;
    if (!Entry) return false;

    const FDTTStationGroup Snapshot = *Group;
    const FDTTStationEntry EntrySnapshot = *Entry;
    if (!RetagStation(Snapshot, EntrySnapshot, FString(), 1)) return false;
    RebuildGroupsFromNames();
    return true;
}

bool UDynamicTimeTableSubsystem::SetTargetSlots(FGuid GroupId, const FString& StationName, int32 TargetSlots)
{
    const FDTTStationGroup* Group = FindGroup(GroupId);
    const FDTTStationEntry* Entry = Group ? FindStationEntry(*Group, StationName) : nullptr;
    if (!Entry) return false;

    const FDTTStationGroup Snapshot = *Group;
    const FDTTStationEntry EntrySnapshot = *Entry;
    if (!RetagStation(Snapshot, EntrySnapshot, Snapshot.GroupName, FMath::Max(1, TargetSlots))) return false;
    RebuildGroupsFromNames();
    return true;
}

void UDynamicTimeTableSubsystem::GetAllVanillaStationNames(TArray<FString>& Out) const
{
    Out.Reset();
    AFGRailroadSubsystem* R = AFGRailroadSubsystem::Get(GetWorld());
    if (!IsValid(R)) return;
    TArray<AFGTrainStationIdentifier*> Ids;
    R->GetAllTrainStations(Ids);
    for (AFGTrainStationIdentifier* Id : Ids) if (IsValid(Id)) Out.AddUnique(Id->GetStationName().ToString());
    Out.Sort();
}

void UDynamicTimeTableSubsystem::GetAllDockingStationNames(TArray<FString>& Out) const
{
    Out.Reset();
    AFGVehicleSubsystem* V = AFGVehicleSubsystem::Get(GetWorld());
    if (!IsValid(V)) return;
    for (AFGDockingStationIdentifier* Id : V->GetDockingStations())
        if (IsValid(Id)) Out.AddUnique(Id->GetStationName().ToString());
    Out.Sort();
}

const TArray<FDTTStationGroup>& UDynamicTimeTableSubsystem::GetStationGroups() const { return StationGroups; }

void UDynamicTimeTableSubsystem::GetGroupRuntimeLoad(FGuid GroupId, TArray<FDTTStationRuntimeLoad>& Out) const
{
    Out.Reset();
    const FDTTStationGroup* G = FindGroup(GroupId);
    if (!G) return;
    const bool bTruck = G->Kind == EDTTGroupKind::Truck;
    AFGVehicleSubsystem* V = bTruck ? AFGVehicleSubsystem::Get(GetWorld()) : nullptr;
    const int32 GroupNetworkID = bTruck ? GetGroupPathNetworkID(*G) : INDEX_NONE;
    for (const FDTTStationEntry& E : G->Stations)
    {
        FDTTStationRuntimeLoad L;
        L.StationName=E.DisplayName; L.AssignedTrains=GetEntryAssignedCount(*G,E);
        L.TargetSlots=FMath::Max(1,E.TargetSlots); L.Ratio=float(L.AssignedTrains)/float(L.TargetSlots);
        L.bOverloaded=L.AssignedTrains>L.TargetSlots;
        // Diagnostic only. An unreachable dock is still handed out as a target, so that the vanilla
        // autopilot reports StationUnreachable and the player learns their group is misconfigured.
        if (bTruck && IsValid(V) && GroupNetworkID != INDEX_NONE)
        {
            const UFGVehiclePathNetwork* DockNetwork = V->FindNetworkByPathNodeGuid(E.DockingPathNodeGuid);
            L.bUnreachable = DockNetwork == nullptr || DockNetwork->GetNetworkID() != GroupNetworkID;
        }
        Out.Add(L);
    }
}
FDTTStationGroup* UDynamicTimeTableSubsystem::FindGroup(FGuid Id){ return StationGroups.FindByPredicate([&](const FDTTStationGroup& G){return G.GroupId==Id;}); }
const FDTTStationGroup* UDynamicTimeTableSubsystem::FindGroup(FGuid Id) const { return StationGroups.FindByPredicate([&](const FDTTStationGroup& G){return G.GroupId==Id;}); }
FDTTStationEntry* UDynamicTimeTableSubsystem::FindStationEntry(FDTTStationGroup& G,const FString& N){return G.Stations.FindByPredicate([&](const FDTTStationEntry&E){return E.DisplayName==N;});}
const FDTTStationEntry* UDynamicTimeTableSubsystem::FindStationEntry(const FDTTStationGroup& G,const FString& N) const{return G.Stations.FindByPredicate([&](const FDTTStationEntry&E){return E.DisplayName==N;});}

AFGTrainStationIdentifier* UDynamicTimeTableSubsystem::FindVanillaStationByName(const FString& Name) const
{
    AFGRailroadSubsystem* R=AFGRailroadSubsystem::Get(GetWorld()); if(!IsValid(R))return nullptr;
    TArray<AFGTrainStationIdentifier*> Ids; R->GetAllTrainStations(Ids);
    for(auto* Id:Ids) if(IsValid(Id)&&Id->GetStationName().ToString()==Name)return Id;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Groups are not stored - they are read out of the station names every time the
// world's stations change. Nothing here can go stale, so there is nothing to
// resolve, prune or migrate at runtime.
// ---------------------------------------------------------------------------

void UDynamicTimeTableSubsystem::RebuildGroupsFromNames()
{
    if (!IsServer()) return;

    TArray<FDTTStationGroup> Rebuilt;
    auto Bucket = [&Rebuilt](EDTTGroupKind Kind, const FString& GroupName) -> FDTTStationGroup&
    {
        if (FDTTStationGroup* Found = Rebuilt.FindByPredicate([&](const FDTTStationGroup& G)
            { return G.Kind == Kind && G.GroupName.Equals(GroupName, ESearchCase::IgnoreCase); }))
        {
            return *Found;
        }
        FDTTStationGroup New;
        New.Kind = Kind;
        New.GroupName = GroupName;
        New.GroupId = DTSMakeGroupId(Kind, GroupName);
        return Rebuilt[Rebuilt.Add(New)];
    };

    if (AFGRailroadSubsystem* Railroad = AFGRailroadSubsystem::Get(GetWorld()))
    {
        TArray<AFGTrainStationIdentifier*> Identifiers;
        Railroad->GetAllTrainStations(Identifiers);
        for (AFGTrainStationIdentifier* Identifier : Identifiers)
        {
            if (!IsValid(Identifier)) continue;
            const FString FullName = Identifier->GetStationName().ToString();
            FString Base, GroupName; int32 Slots = 1;
            if (!ParseStationTag(FullName, Base, GroupName, Slots)) continue;
            FDTTStationEntry Entry;
            Entry.DisplayName = FullName;
            Entry.TargetSlots = Slots;
            Entry.StationIdentifier = Identifier;
            Bucket(EDTTGroupKind::Train, GroupName).Stations.Add(Entry);
        }
    }

    if (AFGVehicleSubsystem* Vehicles = AFGVehicleSubsystem::Get(GetWorld()))
    {
        for (AFGDockingStationIdentifier* Identifier : Vehicles->GetDockingStations())
        {
            if (!IsValid(Identifier)) continue;
            const FString FullName = Identifier->GetStationName().ToString();
            FString Base, GroupName; int32 Slots = 1;
            if (!ParseStationTag(FullName, Base, GroupName, Slots)) continue;
            FDTTStationEntry Entry;
            Entry.DisplayName = FullName;
            Entry.TargetSlots = Slots;
            Entry.DockingStationIdentifier = Identifier;
            Entry.DockingPathNodeGuid = Identifier->GetPathNodeGUID();
            Bucket(EDTTGroupKind::Truck, GroupName).Stations.Add(Entry);
        }
    }

    // PriorityIndex drives the tie-breaking rotation, so it has to mean the same thing from one
    // rebuild to the next - hence a name sort rather than whatever order the subsystem hands out.
    for (FDTTStationGroup& Group : Rebuilt)
    {
        Group.Stations.Sort([](const FDTTStationEntry& A, const FDTTStationEntry& B){ return A.DisplayName < B.DisplayName; });
        RefreshPriorityIndices(Group);
    }

    // A draft that now has a tagged station is a real group; the rest stay visible for the session.
    for (const FDTTStationGroup& Group : Rebuilt) DraftGroups.Remove(Group.GroupId);
    for (const auto& Pair : DraftGroups) Rebuilt.Add(Pair.Value);
    Rebuilt.Sort([](const FDTTStationGroup& A, const FDTTStationGroup& B){ return A.GroupName < B.GroupName; });

    auto SameConfiguration = [](const TArray<FDTTStationGroup>& Left, const TArray<FDTTStationGroup>& Right)
    {
        if (Left.Num() != Right.Num()) return false;
        for (int32 i = 0; i < Left.Num(); ++i)
        {
            if (Left[i].GroupId != Right[i].GroupId) return false;
            if (Left[i].Stations.Num() != Right[i].Stations.Num()) return false;
            for (int32 j = 0; j < Left[i].Stations.Num(); ++j)
            {
                if (Left[i].Stations[j].DisplayName != Right[i].Stations[j].DisplayName) return false;
                if (Left[i].Stations[j].TargetSlots != Right[i].Stations[j].TargetSlots) return false;
            }
        }
        return true;
    };

    const bool bChanged = !SameConfiguration(StationGroups, Rebuilt);
    StationGroups = MoveTemp(Rebuilt);
    LastStationFingerprint = ComputeStationFingerprint();
    bStationFingerprintValid = true;

    if (!bChanged) return;

    // A station that left its group must not keep a booking: it would count against a group it is
    // no longer part of, and nothing would ever release it.
    for (auto It = ActiveBookings.CreateIterator(); It; ++It)
        if (!It.Key().IsValid() || !FindGroupByStation(It.Value().Get())) It.RemoveCurrent();
    for (auto It = TruckBookings.CreateIterator(); It; ++It)
        if (!It.Key().IsValid() || !FindGroupByDock(It.Value().Get())) It.RemoveCurrent();

    // The cursor names a PriorityIndex, and those are renumbered on every rebuild.
    GroupRotationCursor.Reset();

    ++ConfigurationRevision;
    int32 StationCount = 0;
    for (const FDTTStationGroup& Group : StationGroups) StationCount += Group.Stations.Num();
    UE_LOG(LogDynamicTimeTableSubsystem, Display,
        TEXT("GROUPS_REBUILT Groups=%d Stations=%d ConfigurationRevision=%d"),
        StationGroups.Num(), StationCount, ConfigurationRevision);
    OnGroupsChanged.Broadcast(); OnGroupsChangedNative.Broadcast();
}

uint32 UDynamicTimeTableSubsystem::ComputeStationFingerprint() const
{
    // Summed, not combined in order: the subsystems make no promise about the order they hand
    // stations out in, and a reordering must not read as a change.
    uint32 Hash = 0;
    int32 Count = 0;
    if (AFGRailroadSubsystem* Railroad = AFGRailroadSubsystem::Get(GetWorld()))
    {
        TArray<AFGTrainStationIdentifier*> Identifiers;
        Railroad->GetAllTrainStations(Identifiers);
        for (AFGTrainStationIdentifier* Identifier : Identifiers)
        {
            if (!IsValid(Identifier)) continue;
            Hash += GetTypeHash(Identifier->GetStationName().ToString());
            ++Count;
        }
    }
    if (AFGVehicleSubsystem* Vehicles = AFGVehicleSubsystem::Get(GetWorld()))
    {
        for (AFGDockingStationIdentifier* Identifier : Vehicles->GetDockingStations())
        {
            if (!IsValid(Identifier)) continue;
            Hash += GetTypeHash(Identifier->GetStationName().ToString()) * 31u;
            ++Count;
        }
    }
    return HashCombine(Hash, static_cast<uint32>(Count));
}

void UDynamicTimeTableSubsystem::RebuildGroupsIfStationsChanged(const TCHAR* Trigger)
{
    if (!IsServer() || !bWorldStationsReady) return;
    const uint32 Fingerprint = ComputeStationFingerprint();
    if (bStationFingerprintValid && Fingerprint == LastStationFingerprint) return;
    UE_LOG(LogDynamicTimeTableSubsystem, Verbose, TEXT("Station set changed, rebuilding groups. Trigger=%s"), Trigger);
    RebuildGroupsFromNames();
}

void UDynamicTimeTableSubsystem::MigrateSavedGroupsToTags()
{
    if (!IsServer() || !SaveDataActor.IsValid()) return;

    const TArray<FDTTStationGroup> Legacy = SaveDataActor->GetGroups();
    if (Legacy.Num() == 0)
    {
        SyncConfigurationToSaveActor();
        return;
    }

    // If the world has not produced a single station yet, every lookup below would fail and the
    // player's configuration would be thrown away with nothing written in its place. Wait instead.
    if (!HasAnyStations() && MigrationAttempts < 4)
    {
        ++MigrationAttempts;
        bWorldStationsReady = false;
        UE_LOG(LogDynamicTimeTableSubsystem, Warning,
            TEXT("Migration deferred: no stations registered yet. Attempt=%d"), MigrationAttempts);
        return;
    }

    int32 Renamed = 0, AlreadyTagged = 0, Unresolved = 0;
    for (const FDTTStationGroup& Group : Legacy)
    {
        const FString GroupName = SanitizeGroupName(Group.GroupName);
        if (GroupName.IsEmpty()) continue;

        for (const FDTTStationEntry& Entry : Group.Stations)
        {
            FString Current;
            AFGTrainStationIdentifier* Station = nullptr;
            AFGDockingStationIdentifier* Dock = nullptr;

            if (Group.Kind == EDTTGroupKind::Truck)
            {
                Dock = Entry.DockingStationIdentifier.Get();
                if (!IsValid(Dock)) Dock = FindDockByPathNode(Entry.DockingPathNodeGuid);
                if (!IsValid(Dock)) Dock = FindDockByName(Entry.DisplayName);
                if (IsValid(Dock)) Current = Dock->GetStationName().ToString();
            }
            else
            {
                Station = Entry.StationIdentifier.Get();
                if (!IsValid(Station)) Station = FindVanillaStationByName(Entry.DisplayName);
                if (IsValid(Station)) Current = Station->GetStationName().ToString();
            }

            if (!IsValid(Station) && !IsValid(Dock))
            {
                ++Unresolved;
                UE_LOG(LogDynamicTimeTableSubsystem, Warning,
                    TEXT("MIGRATION_SKIPPED Station='%s' Group='%s' Reason=building-gone"),
                    *Entry.DisplayName, *GroupName);
                continue;
            }

            FString Base, ExistingGroup; int32 ExistingSlots = 1;
            if (ParseStationTag(Current, Base, ExistingGroup, ExistingSlots) &&
                ExistingGroup.Equals(GroupName, ESearchCase::IgnoreCase))
            {
                ++AlreadyTagged;
                continue;
            }

            const FString NewName = MakeTaggedName(Current, GroupName, Entry.TargetSlots);
            const bool bOk = Group.Kind == EDTTGroupKind::Truck
                ? RenameDockGuarded(Dock, NewName)
                : RenameStationGuarded(Station, NewName);
            if (bOk)
            {
                ++Renamed;
                UE_LOG(LogDynamicTimeTableSubsystem, Display,
                    TEXT("MIGRATED Group='%s' '%s' -> '%s'"), *GroupName, *Current, *NewName);
            }
            else
            {
                ++Unresolved;
                UE_LOG(LogDynamicTimeTableSubsystem, Warning,
                    TEXT("MIGRATION_FAILED Station='%s' Group='%s' Wanted='%s'"), *Current, *GroupName, *NewName);
            }
        }
    }

    UE_LOG(LogDynamicTimeTableSubsystem, Display,
        TEXT("Migrated dTT configuration to name tags. Groups=%d Renamed=%d AlreadyTagged=%d Unresolved=%d"),
        Legacy.Num(), Renamed, AlreadyTagged, Unresolved);
    SyncConfigurationToSaveActor();
}

bool UDynamicTimeTableSubsystem::HasAnyStations() const
{
    if (AFGRailroadSubsystem* Railroad = AFGRailroadSubsystem::Get(GetWorld()))
    {
        TArray<AFGTrainStationIdentifier*> Identifiers;
        Railroad->GetAllTrainStations(Identifiers);
        for (AFGTrainStationIdentifier* Identifier : Identifiers) if (IsValid(Identifier)) return true;
    }
    if (AFGVehicleSubsystem* Vehicles = AFGVehicleSubsystem::Get(GetWorld()))
    {
        for (AFGDockingStationIdentifier* Identifier : Vehicles->GetDockingStations()) if (IsValid(Identifier)) return true;
    }
    return false;
}

void UDynamicTimeTableSubsystem::HandleStationNameChanged(AFGTrainStationIdentifier* Id)
{
    if (!IsServer() || !IsValid(Id) || InternalNameChanges.Contains(Id)) return;
    UE_LOG(LogDynamicTimeTableSubsystem, Display, TEXT("STATION_RENAMED Kind=Train Station='%s'"),
        *Id->GetStationName().ToString());
    if (!bWorldStationsReady) return;
    RebuildGroupsFromNames();
}
void UDynamicTimeTableSubsystem::RefreshPriorityIndices(FDTTStationGroup& G){for(int32 i=0;i<G.Stations.Num();++i)G.Stations[i].PriorityIndex=i;}
void UDynamicTimeTableSubsystem::ConfigurationChanged(bool Recalc){++ConfigurationRevision;if(Recalc)RecalculateBookingsFromVanilla();else OnGroupsChanged.Broadcast(); OnGroupsChangedNative.Broadcast();}
void UDynamicTimeTableSubsystem::RemoveInvalidBookings(){for(auto It=ActiveBookings.CreateIterator();It;++It)if(!It.Key().IsValid()||!It.Value().IsValid())It.RemoveCurrent();}
void UDynamicTimeTableSubsystem::RemoveInvalidTruckBookings(){for(auto It=TruckBookings.CreateIterator();It;++It)if(!It.Key().IsValid()||!It.Value().IsValid())It.RemoveCurrent();}

void UDynamicTimeTableSubsystem::ReleaseBookingIfReached(AFGTrain* Train,AFGTrainStationIdentifier* Docked)
{
    if(auto* Existing=ActiveBookings.Find(Train)) if(Existing->Get()==Docked){UE_LOG(LogDynamicTimeTableSubsystem,Verbose,TEXT("Released booking: Train='%s' BookedTarget='%s' ActuallyReached='%s'"),*Train->GetTrainName().ToString(),*DTSStationName(Existing->Get()),*DTSStationName(Docked));ActiveBookings.Remove(Train);}
}

FDTTStationGroup* UDynamicTimeTableSubsystem::FindGroupByStation(AFGTrainStationIdentifier* Id){if(!IsValid(Id))return nullptr;return StationGroups.FindByPredicate([&](const FDTTStationGroup&G){return G.Stations.ContainsByPredicate([&](const FDTTStationEntry&E){return E.StationIdentifier.Get()==Id;});});}
const FDTTStationGroup* UDynamicTimeTableSubsystem::FindGroupByStation(AFGTrainStationIdentifier* Id) const{if(!IsValid(Id))return nullptr;return StationGroups.FindByPredicate([&](const FDTTStationGroup&G){return G.Stations.ContainsByPredicate([&](const FDTTStationEntry&E){return E.StationIdentifier.Get()==Id;});});}
int32 UDynamicTimeTableSubsystem::GetAssignedCount(AFGTrainStationIdentifier* Id) const{int32 C=0;for(const auto&P:ActiveBookings)if(P.Key.IsValid()&&P.Value.Get()==Id)++C;return C;}
int32 UDynamicTimeTableSubsystem::GetDockAssignedCount(AFGDockingStationIdentifier* Id) const{int32 C=0;for(const auto&P:TruckBookings)if(P.Key.IsValid()&&P.Value.Get()==Id)++C;return C;}
int32 UDynamicTimeTableSubsystem::GetEntryAssignedCount(const FDTTStationGroup& G,const FDTTStationEntry& E) const{return G.Kind==EDTTGroupKind::Truck?GetDockAssignedCount(E.DockingStationIdentifier.Get()):GetAssignedCount(E.StationIdentifier.Get());}

FDTTStationEntry* UDynamicTimeTableSubsystem::SelectBestStation(FDTTStationGroup& G)
{
    // Reachability is deliberately NOT part of the criterion: a dock the autopilot cannot reach
    // still competes, so the vehicle reports StationUnreachable instead of it silently never being used.
    const bool bTruck=G.Kind==EDTTGroupKind::Truck;
    // The ratio is computed, never stored, so equal load has to be matched with a tolerance -
    // without one, 1/3 and 1/3 can miss each other and the rotation below never engages.
    constexpr double RatioTolerance=1e-6;
    TArray<FDTTStationEntry*> Tied;double BestRatio=TNumericLimits<double>::Max();
    for(auto&E:G.Stations)
    {
        const bool bValid=bTruck?IsValid(E.DockingStationIdentifier.Get()):IsValid(E.StationIdentifier.Get());
        if(!bValid||E.TargetSlots<=0)continue;
        const int32 C=GetEntryAssignedCount(G,E);
        const double R=double(C)/double(E.TargetSlots);
        UE_LOG(LogDynamicTimeTableSubsystem,VeryVerbose,TEXT("Candidate='%s' Assigned=%d TargetSlots=%d Ratio=%.3f"),*E.DisplayName,C,E.TargetSlots,R);
        if(R<BestRatio-RatioTolerance){BestRatio=R;Tied.Reset();Tied.Add(&E);}
        else if(R<=BestRatio+RatioTolerance){Tied.Add(&E);}
    }
    if(Tied.Num()==0)return nullptr;
    if(Tied.Num()==1)return Tied[0];

    // Load stays the primary criterion; only a tie is broken cyclically, so that a group of
    // equally free stations gets used evenly instead of the first one taking every vehicle.
    Tied.Sort([](const FDTTStationEntry& A,const FDTTStationEntry& B){return A.PriorityIndex<B.PriorityIndex;});
    const int32* Found=GroupRotationCursor.Find(G.GroupId);
    const int32 Cursor=Found?*Found:INDEX_NONE;
    FDTTStationEntry* Chosen=nullptr;
    for(FDTTStationEntry* Candidate:Tied)if(Candidate->PriorityIndex>Cursor){Chosen=Candidate;break;}
    if(!Chosen)Chosen=Tied[0];
    GroupRotationCursor.Add(G.GroupId,Chosen->PriorityIndex);
    return Chosen;
}

int32 UDynamicTimeTableSubsystem::CountStopsInGroup(AFGRailroadTimeTable* TimeTable, const FDTTStationGroup& Group) const
{
    if (!IsValid(TimeTable)) return 0;

    TArray<FTimeTableStop> Stops;
    TimeTable->GetStops(Stops);

    int32 Count = 0;
    for (const FTimeTableStop& Stop : Stops)
    {
        AFGTrainStationIdentifier* StopIdentifier = Stop.Station.Get();
        if (!IsValid(StopIdentifier)) continue;

        if (Group.Stations.ContainsByPredicate([StopIdentifier](const FDTTStationEntry& Entry)
        {
            return Entry.StationIdentifier.Get() == StopIdentifier;
        }))
        {
            ++Count;
        }
    }

    return Count;
}

bool UDynamicTimeTableSubsystem::ReplaceCurrentStop(AFGRailroadTimeTable* T,int32 I,AFGTrainStationIdentifier* Id){if(!IsValid(T)||!IsValid(Id)||!T->IsValidStop(I))return false;TArray<FTimeTableStop>S;T->GetStops(S);if(!S.IsValidIndex(I))return false;S[I].Station=Id;InternalTimeTableChanges.Add(T);const bool Ok=T->SetStops(S);InternalTimeTableChanges.Remove(T);return Ok;}
void UDynamicTimeTableSubsystem::ReleaseBooking(AFGTrain* Train,const TCHAR* Reason){if(!IsValid(Train))return;if(auto* Existing=ActiveBookings.Find(Train)){UE_LOG(LogDynamicTimeTableSubsystem,Verbose,TEXT("RELEASED Train='%s' Target='%s' Reason=%s"),*Train->GetTrainName().ToString(),*DTSStationName(Existing->Get()),Reason);ActiveBookings.Remove(Train);OnGroupsChanged.Broadcast();OnGroupsChangedNative.Broadcast();}}
AFGTrain* UDynamicTimeTableSubsystem::FindTrainByTimeTable(AFGRailroadTimeTable* T)const{if(!IsValid(T))return nullptr;AFGRailroadSubsystem*R=AFGRailroadSubsystem::Get(GetWorld());if(!IsValid(R))return nullptr;TArray<AFGTrain*>Trains;R->GetAllTrains(Trains);for(AFGTrain*Train:Trains)if(IsValid(Train)&&Train->GetTimeTable()==T)return Train;return nullptr;}
void UDynamicTimeTableSubsystem::HandleTimeTableChanged(AFGRailroadTimeTable* T)
{
    UWorld* W=GetWorld();if(!IsValid(W)||W->GetNetMode()==NM_Client||!IsValid(T)||InternalTimeTableChanges.Contains(T))return;
    AFGTrain* Train=FindTrainByTimeTable(T);if(!IsValid(Train))return;ReleaseBooking(Train,TEXT("timetable saved"));PendingTimeTableReevaluations.Add(T);SchedulePendingTimeTableReevaluation();
    UE_LOG(LogDynamicTimeTableSubsystem,Display,TEXT("TIMETABLE_CHANGED Train='%s' Stops=%d AutoPilot=%s BookingCleared=true Reevaluation=next-tick"),*Train->GetTrainName().ToString(),T->GetNumStops(),Train->IsSelfDrivingEnabled()?TEXT("true"):TEXT("false"));
}
void UDynamicTimeTableSubsystem::SchedulePendingTimeTableReevaluation()
{
    if(bTimeTableReevaluationScheduled)return;UWorld* W=GetWorld();if(!IsValid(W)||W->GetNetMode()==NM_Client)return;bTimeTableReevaluationScheduled=true;TWeakObjectPtr<UDynamicTimeTableSubsystem> WeakThis(this);
    W->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateLambda([WeakThis](){if(WeakThis.IsValid())WeakThis->ProcessPendingTimeTableReevaluations();}));
}
void UDynamicTimeTableSubsystem::ProcessPendingTimeTableReevaluations()
{
    bTimeTableReevaluationScheduled=false;auto Pending=MoveTemp(PendingTimeTableReevaluations);PendingTimeTableReevaluations.Reset();
    for(const auto& WeakT:Pending){auto* T=WeakT.Get();if(!IsValid(T))continue;auto* Train=FindTrainByTimeTable(T);if(!IsValid(Train)||!Train->IsSelfDrivingEnabled())continue;ProcessCurrentTarget(Train,TEXT("timetable saved"));}
    if(PendingTimeTableReevaluations.Num()>0)SchedulePendingTimeTableReevaluation();
}
void UDynamicTimeTableSubsystem::HandleSelfDrivingChanged(AFGTrain* Train,bool bEnabled)
{
    UWorld* W=GetWorld();if(!IsValid(W)||W->GetNetMode()==NM_Client||!IsValid(Train))return;if(!bEnabled){ReleaseBooking(Train,TEXT("autopilot disabled"));return;}ReleaseBooking(Train,TEXT("autopilot enabled"));ProcessCurrentTarget(Train,TEXT("autopilot enabled"));
}
void UDynamicTimeTableSubsystem::ProcessCurrentTarget(AFGTrain*Train,const TCHAR*Trigger){if(!IsValid(Train)||!Train->IsSelfDrivingEnabled())return;if(!bPersistenceReady)InitializePersistence();RemoveInvalidBookings();AFGRailroadTimeTable*T=Train->GetTimeTable();const int32 I=IsValid(T)?T->GetCurrentStop():INDEX_NONE;if(!IsValid(T)||!T->IsValidStop(I))return;AFGTrainStationIdentifier*Ref=T->GetStop(I).Station.Get();FDTTStationGroup*G=FindGroupByStation(Ref);if(!G||ActiveBookings.Contains(Train))return;const int32 GroupStopCount=CountStopsInGroup(T,*G);if(GroupStopCount>1){ActiveBookings.Add(Train,Ref);const int32 C=GetAssignedCount(Ref);const FDTTStationEntry*E=G->Stations.FindByPredicate([Ref](const FDTTStationEntry&X){return X.StationIdentifier.Get()==Ref;});const int32 Slots=E?FMath::Max(1,E->TargetSlots):1;UE_LOG(LogDynamicTimeTableSubsystem,Verbose,TEXT("BOOKED_FIXED Train='%s' Target='%s' Group='%s' GroupStops=%d Load=%d/%d%s Trigger=%s"),*Train->GetTrainName().ToString(),*DTSStationName(Ref),*G->GroupName,GroupStopCount,C,Slots,C>Slots?TEXT(" OVERLOAD"):TEXT(""),Trigger);OnGroupsChanged.Broadcast();OnGroupsChangedNative.Broadcast();return;}FDTTStationEntry*Sel=SelectBestStation(*G);if(!Sel||!IsValid(Sel->StationIdentifier.Get()))return;AFGTrainStationIdentifier*Id=Sel->StationIdentifier.Get();const bool Ok=ReplaceCurrentStop(T,I,Id);const FString Verified=T->IsValidStop(I)?DTSStationName(T->GetStop(I).Station.Get()):TEXT("<invalid>");if(Ok&&Verified==Sel->DisplayName){ActiveBookings.Add(Train,Id);const int32 C=GetAssignedCount(Id);UE_LOG(LogDynamicTimeTableSubsystem,Verbose,TEXT("ASSIGNED Train='%s' Selected='%s' Load=%d/%d%s Trigger=%s"),*Train->GetTrainName().ToString(),*Sel->DisplayName,C,Sel->TargetSlots,C>Sel->TargetSlots?TEXT(" OVERLOAD"):TEXT(""),Trigger);}OnGroupsChanged.Broadcast();OnGroupsChangedNative.Broadcast();}
void UDynamicTimeTableSubsystem::HandleTrainDocked(AFGTrain*Train,AFGBuildableRailroadStation*Station){if(!IsValid(Train)||!IsValid(Station))return;if(!bPersistenceReady)InitializePersistence();RemoveInvalidBookings();ReleaseBookingIfReached(Train,Station->GetStationIdentifier());ProcessCurrentTarget(Train,TEXT("train docked"));}
void UDynamicTimeTableSubsystem::RecalculateBookingsFromVanilla()
{
    ActiveBookings.Reset();TruckBookings.Reset();
    if(AFGRailroadSubsystem*R=AFGRailroadSubsystem::Get(GetWorld())){TArray<AFGTrain*>Trains;R->GetAllTrains(Trains);for(AFGTrain*Train:Trains){if(!IsValid(Train)||!Train->IsSelfDrivingEnabled())continue;auto*T=Train->GetTimeTable();const int32 I=IsValid(T)?T->GetCurrentStop():INDEX_NONE;if(IsValid(T)&&T->IsValidStop(I)){auto*Target=T->GetStop(I).Station.Get();if(FindGroupByStation(Target))ActiveBookings.Add(Train,Target);}}}
    if(AFGVehicleSubsystem*V=AFGVehicleSubsystem::Get(GetWorld())){for(AFGWheeledVehicleIdentifier*Vehicle:V->GetAllVehicles()){if(!IsValid(Vehicle)||!Vehicle->IsAutopilotEnabled())continue;const TArray<FGuid>&Route=Vehicle->GetVehicleRoute();const int32 I=Vehicle->GetCurrentTargetWaypointIndex();if(!Route.IsValidIndex(I))continue;AFGDockingStationIdentifier*Dock=FindDockByPathNode(Route[I]);if(FindGroupByDock(Dock))TruckBookings.Add(Vehicle,Dock);}}
    UE_LOG(LogDynamicTimeTableSubsystem,Display,TEXT("Recalculated %d train and %d truck booking(s) from Vanilla routes."),ActiveBookings.Num(),TruckBookings.Num());OnGroupsChanged.Broadcast(); OnGroupsChangedNative.Broadcast();
}


// ---------------------------------------------------------------------------
// Truck side. Docking stations and vehicle routes mirror train stations and
// timetables one for one, so the balancing rule itself is shared with the trains
// (SelectBestStation); only the resolvers and the write path differ.
// ---------------------------------------------------------------------------

AFGDockingStationIdentifier* UDynamicTimeTableSubsystem::FindDockByName(const FString& Name) const
{
    AFGVehicleSubsystem* V = AFGVehicleSubsystem::Get(GetWorld());
    if (!IsValid(V)) return nullptr;
    for (AFGDockingStationIdentifier* Id : V->GetDockingStations())
        if (IsValid(Id) && Id->GetStationName().ToString() == Name) return Id;
    return nullptr;
}

AFGDockingStationIdentifier* UDynamicTimeTableSubsystem::FindDockByPathNode(const FGuid& PathNodeGuid) const
{
    if (!PathNodeGuid.IsValid()) return nullptr;
    AFGVehicleSubsystem* V = AFGVehicleSubsystem::Get(GetWorld());
    if (!IsValid(V)) return nullptr;
    return V->FindDockingStationIdentifierForPathNodeGuid(PathNodeGuid);
}

FDTTStationGroup* UDynamicTimeTableSubsystem::FindGroupByDock(AFGDockingStationIdentifier* Id)
{
    if (!IsValid(Id)) return nullptr;
    return StationGroups.FindByPredicate([&](const FDTTStationGroup& G)
    {
        return G.Kind == EDTTGroupKind::Truck && G.Stations.ContainsByPredicate(
            [&](const FDTTStationEntry& E){ return E.DockingStationIdentifier.Get() == Id; });
    });
}

const FDTTStationGroup* UDynamicTimeTableSubsystem::FindGroupByDock(AFGDockingStationIdentifier* Id) const
{
    if (!IsValid(Id)) return nullptr;
    return StationGroups.FindByPredicate([&](const FDTTStationGroup& G)
    {
        return G.Kind == EDTTGroupKind::Truck && G.Stations.ContainsByPredicate(
            [&](const FDTTStationEntry& E){ return E.DockingStationIdentifier.Get() == Id; });
    });
}

void UDynamicTimeTableSubsystem::HandleDockingStationNameChanged(AFGDockingStationIdentifier* Id)
{
    if (!IsServer() || !IsValid(Id) || InternalNameChanges.Contains(Id)) return;
    UE_LOG(LogDynamicTimeTableSubsystem, Display, TEXT("STATION_RENAMED Kind=Truck Station='%s'"), *DTSDockName(Id));
    if (!bWorldStationsReady) return;
    RebuildGroupsFromNames();
}

int32 UDynamicTimeTableSubsystem::GetGroupPathNetworkID(const FDTTStationGroup& G) const
{
    if (G.Kind != EDTTGroupKind::Truck) return INDEX_NONE;
    AFGVehicleSubsystem* V = AFGVehicleSubsystem::Get(GetWorld());
    if (!IsValid(V)) return INDEX_NONE;
    for (AFGWheeledVehicleIdentifier* Vehicle : V->GetAllVehicles())
    {
        if (!IsValid(Vehicle) || !Vehicle->IsAutopilotEnabled()) continue;
        if (CountWaypointsInGroup(Vehicle, G) == 0) continue;
        if (const UFGVehiclePathNetwork* Network = V->FindNetworkByPathNodeGuid(Vehicle->GetCurrentFromPathNodeGUID()))
            return Network->GetNetworkID();
    }
    return INDEX_NONE;
}

int32 UDynamicTimeTableSubsystem::CountWaypointsInGroup(AFGWheeledVehicleIdentifier* Vehicle, const FDTTStationGroup& Group) const
{
    if (!IsValid(Vehicle)) return 0;
    int32 Count = 0;
    for (const FGuid& Node : Vehicle->GetVehicleRoute())
    {
        if (!Node.IsValid()) continue;
        if (Group.Stations.ContainsByPredicate([&Node](const FDTTStationEntry& E){ return E.DockingPathNodeGuid == Node; }))
            ++Count;
    }
    return Count;
}

bool UDynamicTimeTableSubsystem::ReplaceCurrentWaypoint(AFGWheeledVehicleIdentifier* Vehicle, int32 WaypointIndex, const FGuid& NewPathNodeGuid)
{
    if (!IsValid(Vehicle) || !NewPathNodeGuid.IsValid()) return false;
    TArray<FGuid> Route = Vehicle->GetVehicleRoute();
    if (!Route.IsValidIndex(WaypointIndex)) return false;
    if (Route[WaypointIndex] == NewPathNodeGuid) return true;
    Route[WaypointIndex] = NewPathNodeGuid;
    // SetVehicleRoute is hooked by this mod, so the write has to be marked as ours.
    InternalRouteChanges.Add(Vehicle);
    Vehicle->SetVehicleRoute(Route);
    InternalRouteChanges.Remove(Vehicle);
    return Vehicle->GetVehicleRoute().IsValidIndex(WaypointIndex) && Vehicle->GetVehicleRoute()[WaypointIndex] == NewPathNodeGuid;
}

void UDynamicTimeTableSubsystem::ReleaseTruckBooking(AFGWheeledVehicleIdentifier* Vehicle, const TCHAR* Reason)
{
    if (!IsValid(Vehicle)) return;
    if (auto* Existing = TruckBookings.Find(Vehicle))
    {
        UE_LOG(LogDynamicTimeTableSubsystem, Verbose, TEXT("RELEASED Truck='%s' Target='%s' Reason=%s"), *DTSVehicleName(Vehicle), *DTSDockName(Existing->Get()), Reason);
        TruckBookings.Remove(Vehicle);
        OnGroupsChanged.Broadcast(); OnGroupsChangedNative.Broadcast();
    }
}

void UDynamicTimeTableSubsystem::SchedulePendingRouteReevaluation()
{
    if (bRouteReevaluationScheduled) return;
    UWorld* W = GetWorld();
    if (!IsValid(W) || !IsServer()) return;
    bRouteReevaluationScheduled = true;
    TWeakObjectPtr<UDynamicTimeTableSubsystem> WeakThis(this);
    W->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateLambda([WeakThis]()
    {
        if (WeakThis.IsValid()) WeakThis->ProcessPendingRouteReevaluations();
    }));
}

void UDynamicTimeTableSubsystem::ProcessPendingRouteReevaluations()
{
    bRouteReevaluationScheduled = false;
    auto Pending = MoveTemp(PendingRouteReevaluations);
    PendingRouteReevaluations.Reset();
    for (const auto& WeakVehicle : Pending)
    {
        AFGWheeledVehicleIdentifier* Vehicle = WeakVehicle.Get();
        if (!IsValid(Vehicle) || !Vehicle->IsAutopilotEnabled()) continue;
        ProcessCurrentTruckTarget(Vehicle, TEXT("deferred"));
    }
    if (PendingRouteReevaluations.Num() > 0) SchedulePendingRouteReevaluation();
}

void UDynamicTimeTableSubsystem::ProcessCurrentTruckTarget(AFGWheeledVehicleIdentifier* Vehicle, const TCHAR* Trigger)
{
    if (!IsValid(Vehicle) || !Vehicle->IsAutopilotEnabled()) return;
    if (!bPersistenceReady) InitializePersistence();
    RemoveInvalidTruckBookings();

    const TArray<FGuid>& Route = Vehicle->GetVehicleRoute();
    const int32 Index = Vehicle->GetCurrentTargetWaypointIndex();
    // Covers both INDEX_NONE and an index left over from a route the player has since shortened.
    if (!Route.IsValidIndex(Index)) return;

    AFGDockingStationIdentifier* Current = FindDockByPathNode(Route[Index]);
    FDTTStationGroup* Group = FindGroupByDock(Current);
    if (!Group) return;

    // A booking always names the dock the vehicle is heading for right now. Anything else is
    // stale and gets dropped here rather than blocking the vehicle forever - a booking that
    // could not be released was what made the mod go permanently inert.
    if (auto* Existing = TruckBookings.Find(Vehicle))
    {
        if (Existing->Get() == Current) return;
        ReleaseTruckBooking(Vehicle, TEXT("target changed"));
    }

    // The player listed several stations of the group by hand - that is an explicit route, not
    // something to balance. Same rule as for trains.
    const int32 GroupStopCount = CountWaypointsInGroup(Vehicle, *Group);
    if (GroupStopCount > 1)
    {
        TruckBookings.Add(Vehicle, Current);
        const int32 C = GetDockAssignedCount(Current);
        const FDTTStationEntry* E = Group->Stations.FindByPredicate([Current](const FDTTStationEntry& X){ return X.DockingStationIdentifier.Get() == Current; });
        const int32 Slots = E ? FMath::Max(1, E->TargetSlots) : 1;
        UE_LOG(LogDynamicTimeTableSubsystem, Verbose, TEXT("BOOKED_FIXED Truck='%s' Target='%s' Group='%s' GroupStops=%d Load=%d/%d%s Trigger=%s"),
            *DTSVehicleName(Vehicle), *DTSDockName(Current), *Group->GroupName, GroupStopCount, C, Slots, C > Slots ? TEXT(" OVERLOAD") : TEXT(""), Trigger);
        OnGroupsChanged.Broadcast(); OnGroupsChangedNative.Broadcast();
        return;
    }

    FDTTStationEntry* Selected = SelectBestStation(*Group);
    if (!Selected || !IsValid(Selected->DockingStationIdentifier.Get())) return;
    AFGDockingStationIdentifier* Dock = Selected->DockingStationIdentifier.Get();

    // Rewriting the route invalidates the autopilot's cached path, so skip the write when the
    // vehicle is already heading where it should.
    const bool bOk = Dock == Current || ReplaceCurrentWaypoint(Vehicle, Index, Dock->GetPathNodeGUID());
    if (bOk)
    {
        TruckBookings.Add(Vehicle, Dock);
        const int32 C = GetDockAssignedCount(Dock);
        UE_LOG(LogDynamicTimeTableSubsystem, Verbose, TEXT("ASSIGNED Truck='%s' Selected='%s' Load=%d/%d%s Trigger=%s"),
            *DTSVehicleName(Vehicle), *Selected->DisplayName, C, Selected->TargetSlots, C > Selected->TargetSlots ? TEXT(" OVERLOAD") : TEXT(""), Trigger);
    }
    OnGroupsChanged.Broadcast(); OnGroupsChangedNative.Broadcast();
}

void UDynamicTimeTableSubsystem::StartTruckPolling()
{
    UWorld* World = GetWorld();
    if (!IsValid(World) || !IsServer()) return;
    World->GetTimerManager().ClearTimer(TruckPollTimerHandle);
    World->GetTimerManager().SetTimer(TruckPollTimerHandle, this,
        &UDynamicTimeTableSubsystem::PollTruckStates, 1.0f, true);
    UE_LOG(LogDynamicTimeTableSubsystem, Display, TEXT("Truck polling started at 1 Hz."));
}

void UDynamicTimeTableSubsystem::PollTruckStates()
{
    if (!IsServer()) return;
    AFGVehicleSubsystem* V = AFGVehicleSubsystem::Get(GetWorld());
    if (!IsValid(V)) return;

    // Building or demolishing a station fires no hook at all, and the train half has no timer of
    // its own, so both kinds are checked here.
    RebuildGroupsIfStationsChanged(TEXT("poll"));

    for (auto It = TruckPollStates.CreateIterator(); It; ++It)
        if (!It.Key().IsValid()) It.RemoveCurrent();

    for (AFGWheeledVehicleIdentifier* Vehicle : V->GetAllVehicles())
    {
        if (!IsValid(Vehicle)) continue;
        if (!Vehicle->IsAutopilotEnabled())
        {
            TruckPollStates.Remove(Vehicle);
            continue;
        }

        const int32 TargetIndex = Vehicle->GetCurrentTargetWaypointIndex();
        const bool bDocking = Vehicle->IsCurrentlyDocking();
        FDTTTruckPollState* Previous = TruckPollStates.Find(Vehicle);

        // First sighting only records. Acting here would redirect a vehicle that is already most
        // of the way to its target, every time the world loads.
        if (!Previous)
        {
            TruckPollStates.Add(Vehicle, FDTTTruckPollState{TargetIndex, bDocking});
            continue;
        }

        // Arrival frees the slot, but does not re-evaluate: the target is still the dock the
        // vehicle is standing on, and rewriting that waypoint would send it away again.
        if (bDocking && !Previous->bDocking)
        {
            ReleaseTruckBooking(Vehicle, TEXT("truck docked"));
        }

        // The autopilot moving on to the next waypoint is the moment to substitute.
        if (TargetIndex != Previous->TargetIndex)
        {
            ProcessCurrentTruckTarget(Vehicle, TEXT("waypoint changed"));
        }

        Previous->TargetIndex = TargetIndex;
        Previous->bDocking = bDocking;
    }
}

void UDynamicTimeTableSubsystem::HandleVehicleRouteChanged(AFGWheeledVehicleIdentifier* Vehicle)
{
    if (!IsServer() || !IsValid(Vehicle) || InternalRouteChanges.Contains(Vehicle)) return;
    ReleaseTruckBooking(Vehicle, TEXT("route saved"));
    PendingRouteReevaluations.Add(Vehicle);
    SchedulePendingRouteReevaluation();
    UE_LOG(LogDynamicTimeTableSubsystem, Display, TEXT("ROUTE_CHANGED Truck='%s' Waypoints=%d AutoPilot=%s BookingCleared=true Reevaluation=next-tick"),
        *DTSVehicleName(Vehicle), Vehicle->GetVehicleRoute().Num(), Vehicle->IsAutopilotEnabled() ? TEXT("true") : TEXT("false"));
}

void UDynamicTimeTableSubsystem::HandleAutopilotChanged(AFGWheeledVehicleIdentifier* Vehicle, bool bEnabled)
{
    if (!IsServer() || !IsValid(Vehicle)) return;
    ReleaseTruckBooking(Vehicle, bEnabled ? TEXT("autopilot enabled") : TEXT("autopilot disabled"));
    if (!bEnabled) return;
    PendingRouteReevaluations.Add(Vehicle);
    SchedulePendingRouteReevaluation();
}

void UDynamicTimeTableSubsystem::ScheduleWorldStationsReady()
{
    UWorld* W = GetWorld();
    if (bWorldStationsReady || !IsValid(W) || !IsServer()) return;
    // Not a next-tick timer on purpose. At OnWorldBeginPlay the railroad and vehicle subsystems
    // have not registered their identifiers yet, and the migration below only gets one chance to
    // read the player's old configuration before it is replaced by tags.
    TWeakObjectPtr<UDynamicTimeTableSubsystem> WeakThis(this);
    FTimerHandle Handle;
    W->GetTimerManager().SetTimer(Handle, FTimerDelegate::CreateLambda([WeakThis]()
    {
        if (!WeakThis.IsValid()) return;
        WeakThis->bWorldStationsReady = true;
        WeakThis->MigrateSavedGroupsToTags();
        // Migration clears the flag again when it wants another go at a world that is still empty.
        if (!WeakThis->bWorldStationsReady) { WeakThis->ScheduleWorldStationsReady(); return; }
        WeakThis->RebuildGroupsFromNames();
        WeakThis->RecalculateBookingsFromVanilla();
    }), 15.0f, false);
}
