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
#include "WheeledVehicles/FGWheeledVehicle.h"
#include "WheeledVehicles/FGWheeledVehicleIdentifier.h"
#include "Buildables/FGBuildableDockingStation.h"
#include "FGInventoryComponent.h"
#include "FGBlueprintFunctionLibrary.h"
#include "Resources/FGItemDescriptor.h"
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

    TArray<FString> DTSExtractParenthesizedTags(const FString& Text)
    {
        TArray<FString> Tags;
        int32 SearchFrom = 0;
        while (SearchFrom < Text.Len())
        {
            const int32 Open = Text.Find(TEXT("("), ESearchCase::CaseSensitive,
                ESearchDir::FromStart, SearchFrom);
            if (Open == INDEX_NONE) break;
            const int32 Close = Text.Find(TEXT(")"), ESearchCase::CaseSensitive,
                ESearchDir::FromStart, Open + 1);
            if (Close == INDEX_NONE) break;
            if (!Text.Mid(Open + 1, Close - Open - 1).TrimStartAndEnd().IsEmpty())
            {
                Tags.Add(Text.Mid(Open, Close - Open + 1));
            }
            SearchFrom = Close + 1;
        }
        return Tags;
    }

    void DTSAddUniqueTag(TArray<FString>& Tags, const FString& Candidate)
    {
        if (!Tags.ContainsByPredicate([&](const FString& Existing)
            { return Existing.Equals(Candidate, ESearchCase::IgnoreCase); }))
        {
            Tags.Add(Candidate);
        }
    }

    FString DTSNormalizeSpaces(FString Text)
    {
        Text.ReplaceInline(TEXT("\t"), TEXT(" "));
        while (Text.ReplaceInline(TEXT("  "), TEXT(" ")) > 0) {}
        return Text.TrimStartAndEnd();
    }

    FString DTSRemoveTags(FString Text, const TArray<FString>& Tags)
    {
        for (const FString& Tag : Tags)
            Text.ReplaceInline(*Tag, TEXT(" "), ESearchCase::IgnoreCase);
        return DTSNormalizeSpaces(MoveTemp(Text));
    }

    TArray<FString> DTSTakeLeadingParenthesizedTags(FString& Text)
    {
        TArray<FString> Tags;
        Text = Text.TrimStartAndEnd();
        while (Text.StartsWith(TEXT("(")))
        {
            const int32 Close = Text.Find(TEXT(")"), ESearchCase::CaseSensitive,
                ESearchDir::FromStart, 1);
            if (Close == INDEX_NONE || Text.Mid(1, Close - 1).TrimStartAndEnd().IsEmpty()) break;
            DTSAddUniqueTag(Tags, Text.Left(Close + 1));
            Text = Text.Mid(Close + 1).TrimStartAndEnd();
        }
        return Tags;
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

    // New canonical form stores only the base group identity inside square brackets and writes
    // inherited round tags immediately after it: "Dock [Depot] (Iron)". Read the old
    // "[Depot (Iron)] (Iron)" form too, then expose the full display name to the UI either way.
    TArray<FString> InheritedTags;
    for (const FString& Tag : DTSExtractParenthesizedTags(Inner))
        DTSAddUniqueTag(InheritedTags, Tag);
    FString GroupBase = DTSRemoveTags(Inner, InheritedTags);
    if (GroupBase.IsEmpty()) GroupBase = Inner;

    FString After = FullName.Mid(Close + 1).TrimStartAndEnd();
    for (const FString& Tag : DTSTakeLeadingParenthesizedTags(After))
        DTSAddUniqueTag(InheritedTags, Tag);

    OutGroupName = GroupBase;
    if (InheritedTags.Num() > 0)
        OutGroupName += FString::Printf(TEXT(" %s"), *FString::Join(InheritedTags, TEXT(" ")));

    // Once a group owns a tag, an identical station-owned copy is redundant. Canonicalization
    // deliberately folds it into the inherited suffix so the visible station name has one copy.
    OutBaseName = DTSRemoveTags(FullName.Left(Open), InheritedTags);
    if (!After.IsEmpty())
    {
        OutBaseName = OutBaseName.IsEmpty() ? After
            : FString::Printf(TEXT("%s %s"), *OutBaseName, *After);
    }
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
    const FString Group = SanitizeGroupName(GroupName);
    FString Base = StripStationTag(BaseName);
    if (Group.IsEmpty()) return Base;

    TArray<FString> InheritedTagArray;
    for (const FString& InheritedTag : DTSExtractParenthesizedTags(Group))
        DTSAddUniqueTag(InheritedTagArray, InheritedTag);
    FString GroupBase = DTSRemoveTags(Group, InheritedTagArray);
    if (GroupBase.IsEmpty()) GroupBase = Group;
    Base = DTSRemoveTags(MoveTemp(Base), InheritedTagArray);

    const int32 Slots = FMath::Max(1, TargetSlots);
    const FString Tag = Slots > 1 ? FString::Printf(TEXT("[%s,%d]"), *GroupBase, Slots)
                                  : FString::Printf(TEXT("[%s]"), *GroupBase);
    const FString InheritedTags = FString::Join(InheritedTagArray, TEXT(" "));
    FString Result = Base.IsEmpty() ? Tag : FString::Printf(TEXT("%s %s"), *Base, *Tag);
    if (!InheritedTags.IsEmpty()) Result += FString::Printf(TEXT(" %s"), *InheritedTags);
    return Result;
}

bool UDynamicTimeTableSubsystem::ParseDispatchTag(const FString& FullName, FString& OutBaseName, FString& OutResourceId, EDTTDispatchRole& OutRole, int32& OutThreshold)
{
    OutBaseName = FullName.TrimStartAndEnd();
    OutResourceId.Reset();
    OutRole = EDTTDispatchRole::None;
    OutThreshold = 0;

    // Walk parenthesized suffixes backwards. This keeps the independent (F) marker composable
    // with dispatch names in either order: "Dock (Iron) (F)" and "Dock (F) (Iron)".
    int32 SearchEnd = FullName.Len();
    while (SearchEnd > 0)
    {
        int32 Close = INDEX_NONE;
        if (!FullName.Left(SearchEnd).FindLastChar(TEXT(')'), Close)) break;
        int32 Open = INDEX_NONE;
        if (!FullName.Left(Close).FindLastChar(TEXT('('), Open)) break;

        const FString Inner = FullName.Mid(Open + 1, Close - Open - 1).TrimStartAndEnd();
        FString Resource;
        EDTTDispatchRole Role = EDTTDispatchRole::None;
        int32 Threshold = 0;
        if (Inner.EndsWith(TEXT("+")))
        {
            Resource = Inner.LeftChop(1).TrimStartAndEnd();
            if (!Resource.IsEmpty()) Role = EDTTDispatchRole::Producer;
        }
        else
        {
            // The threshold is whatever follows the last '-', and only if it is a plain
            // non-negative number, so a resource named "Iron Ore" still works.
            int32 Dash = INDEX_NONE;
            if (Inner.FindLastChar(TEXT('-'), Dash))
            {
                const FString Tail = Inner.Mid(Dash + 1).TrimStartAndEnd();
                Resource = Inner.Left(Dash).TrimStartAndEnd();
                if (!Resource.IsEmpty() && DTSIsAllDigits(Tail))
                {
                    Role = EDTTDispatchRole::Requester;
                    Threshold = FCString::Atoi(*Tail);
                }
            }

            // A neutral tag delegates its role to the station's live LOAD/UNLOAD switch. Keep
            // explicit +/- tags compatible so existing worlds do not need to rename stations.
            if (Role == EDTTDispatchRole::None && !Inner.Equals(TEXT("F"), ESearchCase::IgnoreCase))
            {
                Resource = Inner;
                if (!Resource.IsEmpty())
                {
                    Role = EDTTDispatchRole::ByStationMode;
                    Threshold = DefaultDispatchRequesterThresholdStacks;
                }
            }
        }

        if (Role != EDTTDispatchRole::None)
        {
            OutBaseName = (FullName.Left(Open) + FullName.Mid(Close + 1)).TrimStartAndEnd();
            OutResourceId = Resource;
            OutRole = Role;
            OutThreshold = Threshold;
            return true;
        }
        SearchEnd = Open;
    }
    return false;
}

FString UDynamicTimeTableSubsystem::StripDispatchTag(const FString& FullName)
{
    FString Base, ResourceId; EDTTDispatchRole Role; int32 Threshold;
    ParseDispatchTag(FullName, Base, ResourceId, Role, Threshold);
    return Base;
}

FString UDynamicTimeTableSubsystem::MakeDispatchTaggedName(const FString& BaseName, const FString& ResourceId, EDTTDispatchRole Role, int32 Threshold)
{
    const FString Base = StripDispatchTag(BaseName);
    // Reuses SanitizeGroupName: '(' / ')' / '-' would break this tag's own syntax the same way
    // '[' / ']' / ',' would break the balancing tag's, so the same strip-to-space rule applies.
    FString Resource = SanitizeGroupName(ResourceId).Replace(TEXT("("), TEXT(" ")).Replace(TEXT(")"), TEXT(" ")).Replace(TEXT("-"), TEXT(" ")).Replace(TEXT("+"), TEXT(" ")).TrimStartAndEnd();
    if (Resource.IsEmpty() || Role == EDTTDispatchRole::None) return Base;

    const FString Tag = Role == EDTTDispatchRole::Producer
        ? FString::Printf(TEXT("(%s+)"), *Resource)
        : Role == EDTTDispatchRole::Requester
            ? FString::Printf(TEXT("(%s-%d)"), *Resource, FMath::Max(0, Threshold))
            : FString::Printf(TEXT("(%s)"), *Resource);
    return Base.IsEmpty() ? Tag : FString::Printf(TEXT("%s %s"), *Base, *Tag);
}

bool UDynamicTimeTableSubsystem::ParseFuelStopTag(const FString& FullName, FString& OutBaseName)
{
    OutBaseName = FullName.TrimStartAndEnd();
    static const FString Tag = TEXT("(F)");
    const int32 TagIndex = FullName.Find(Tag, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
    if (TagIndex == INDEX_NONE) return false;
    OutBaseName = (FullName.Left(TagIndex) + FullName.Mid(TagIndex + Tag.Len())).TrimStartAndEnd();
    return true;
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
    DispatchStations.Reset();
    DispatchFuelStopGuids.Reset();
    RequesterClaims.Reset();
    RequesterClaimedAtWorldSeconds.Reset();
    ItemClassByNameCache.Reset();
    DispatchTruckAnchors.Reset();
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

        // Restored as-is and validated lazily: UpdateRememberedAnchors drops an anchor whose producer
        // no longer matches Route[0], and RebuildGroupsFromNames one whose (F) lost its tag. That
        // rebuild only runs once the world's stations are readable, so an empty fuel-stop set at
        // this early point cannot wipe what was just loaded.
        int32 Restored = 0;
        for (const FDTTSavedTruckAnchors& Saved : Found->GetTruckAnchors())
        {
            if (!IsValid(Saved.Vehicle.Get()) || !Saved.Anchors.ProducerGuid.IsValid()) continue;
            DispatchTruckAnchors.Add(Saved.Vehicle.Get(), Saved.Anchors);
            ++Restored;
        }
        UE_LOG(LogDynamicTimeTableSubsystem, Display,
            TEXT("DISPATCH_ANCHORS_RESTORED Count=%d Saved=%d"), Restored, Found->GetTruckAnchors().Num());
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
    // The save actor stores no groups: the configuration lives in the station names, which the
    // game saves itself. It is kept because the class is referenced by existing saves, and since
    // version 5 it also carries the dispatch fuel anchors (see ExportTruckAnchors).
    if (SaveDataActor.IsValid() && IsServer())
    {
        SaveDataActor->SetGroups(TArray<FDTTStationGroup>());
        SaveDataActor->SetDataVersion(5);
    }
}

void UDynamicTimeTableSubsystem::ExportTruckAnchors(TArray<FDTTSavedTruckAnchors>& OutAnchors) const
{
    OutAnchors.Reset();
    for (const auto& Pair : DispatchTruckAnchors)
    {
        if (!Pair.Key.IsValid() || !Pair.Value.ProducerGuid.IsValid()) continue;
        FDTTSavedTruckAnchors& Saved = OutAnchors.AddDefaulted_GetRef();
        Saved.Vehicle = Pair.Key.Get();
        Saved.Anchors = Pair.Value;
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
        // Snapshots expose StationGroups, not DraftGroups directly. Rebuild merges the new draft
        // into that public array before the RCO sends its immediate post-create snapshot.
        RebuildGroupsFromNames();
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
        // Keep StationGroups in sync for the immediate post-rename snapshot, just as on create.
        RebuildGroupsFromNames();
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

void UDynamicTimeTableSubsystem::GetDispatchStatusLines(TArray<FString>& OutLines, const FString& ResourceFilter) const
{
    OutLines.Reset();
    OutLines.Add(ResourceFilter.IsEmpty()
        ? FString::Printf(TEXT("dTT dispatch: stations=%d fuel-stops=%d active-claims=%d"),
            DispatchStations.Num(), DispatchFuelStopGuids.Num(), RequesterClaims.Num())
        : FString::Printf(TEXT("dTT dispatch: resource='%s'"), *ResourceFilter));

    TArray<FDTTDispatchStation> Stations = DispatchStations;
    if (!ResourceFilter.IsEmpty())
    {
        Stations.RemoveAll([&](const FDTTDispatchStation& Station)
        {
            return !Station.ResourceId.Equals(ResourceFilter, ESearchCase::IgnoreCase);
        });
    }
    Stations.Sort([](const FDTTDispatchStation& A, const FDTTDispatchStation& B)
    {
        const int32 ResourceCompare = A.ResourceId.Compare(B.ResourceId, ESearchCase::IgnoreCase);
        if (ResourceCompare != 0) return ResourceCompare < 0;
        if (A.Role != B.Role) return static_cast<uint8>(A.Role) < static_cast<uint8>(B.Role);
        return A.DisplayName < B.DisplayName;
    });

    for (const FDTTDispatchStation& Station : Stations)
    {
        const AFGDockingStationIdentifier* Identifier = Station.DockingStationIdentifier.Get();
        const AFGBuildableDockingStation* Dock = IsValid(Identifier) ? Identifier->GetStation() : nullptr;
        const TCHAR* ActualMode = !IsValid(Dock) ? TEXT("unavailable")
            : (Dock->GetIsInLoadMode() ? TEXT("LOAD") : TEXT("UNLOAD"));
        const bool bModeMatches = DoesDispatchStationMatchMode(Station);
        const TSubclassOf<UFGItemDescriptor> ItemClass = ResolveItemClassForResource(Station.ResourceId);
        const int32 StockStacks = ItemClass
            ? ReadDockStockInStacks(Station.DockingStationIdentifier.Get(), ItemClass) : INDEX_NONE;
        const FString BaseName = StripDispatchTag(Station.DisplayName);

        const EDTTDispatchRole EffectiveRole = GetEffectiveDispatchRole(Station);
        if (EffectiveRole == EDTTDispatchRole::Producer)
        {
            OutLines.Add(FString::Printf(
                TEXT("%s | Producer | %s | mode=%s active=%s | stock=%s"),
                *Station.ResourceId, *BaseName, ActualMode, bModeMatches ? TEXT("yes") : TEXT("no"),
                StockStacks >= 0 ? *FString::Printf(TEXT("%d stacks"), StockStacks) : TEXT("unknown")));
            continue;
        }

        FString Claim = TEXT("free");
        if (const TWeakObjectPtr<AFGWheeledVehicleIdentifier>* Owner =
            RequesterClaims.Find(Station.DockingPathNodeGuid))
        {
            Claim = Owner->IsValid() ? DTSVehicleName(Owner->Get()) : TEXT("<invalid vehicle>");
        }
        const bool bOpen = bModeMatches && StockStacks >= 0 && StockStacks < Station.Threshold;
        OutLines.Add(FString::Printf(
            TEXT("%s | Requester | %s | mode=%s active=%s | stock=%s/%d open=%s | claim=%s"),
            *Station.ResourceId, *BaseName, ActualMode, bModeMatches ? TEXT("yes") : TEXT("no"),
            StockStacks >= 0 ? *FString::FromInt(StockStacks) : TEXT("unknown"), Station.Threshold,
            bOpen ? TEXT("yes") : TEXT("no"), *Claim));
    }

    // Fuel stops aren't tied to one resource - every dispatch truck can borrow any of them - so a
    // per-resource filter drops this section too, since it is the same regardless of which
    // resource was asked about and chat has no scrollback to spare on it.
    if (ResourceFilter.IsEmpty())
    {
        TArray<FGuid> FuelStops = DispatchFuelStopGuids.Array();
        FuelStops.Sort([](const FGuid& A, const FGuid& B){ return A.ToString() < B.ToString(); });
        for (const FGuid& FuelGuid : FuelStops)
        {
            const AFGDockingStationIdentifier* FuelDock = FindDockByPathNode(FuelGuid);
            const FString Name = IsValid(FuelDock)
                ? FuelDock->GetStationName().ToString() : TEXT("<missing dock>");
            OutLines.Add(FString::Printf(TEXT("Fuel (F) | %s | guid=%s"),
                *Name, *FuelGuid.ToString()));
        }
    }

    if (!ResourceFilter.IsEmpty() && Stations.Num() == 0)
    {
        OutLines.Add(FString::Printf(
            TEXT("No dispatch stations tagged with resource '%s'."), *ResourceFilter));
    }
    else if (DispatchStations.Num() == 0 && DispatchFuelStopGuids.Num() == 0)
    {
        OutLines.Add(TEXT("No dispatch tags. Use (Resource) on LOAD/UNLOAD stations, optional "
            "(Resource-N) to override a requester threshold, and optional (F) on a fuel stop."));
    }
}

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
            FString FullName = Identifier->GetStationName().ToString();
            FString Base, GroupName; int32 Slots = 1;
            if (!ParseStationTag(FullName, Base, GroupName, Slots)) continue;
            const FString CanonicalName = MakeTaggedName(FullName, GroupName, Slots);
            if (CanonicalName != FullName && RenameStationGuarded(Identifier, CanonicalName))
            {
                FullName = CanonicalName;
                UE_LOG(LogDynamicTimeTableSubsystem, Verbose,
                    TEXT("GROUP_TAGS_INHERITED Kind=Train Group='%s' Station='%s'"),
                    *GroupName, *FullName);
            }
            FDTTStationEntry Entry;
            Entry.DisplayName = FullName;
            Entry.TargetSlots = Slots;
            Entry.StationIdentifier = Identifier;
            Bucket(EDTTGroupKind::Train, GroupName).Stations.Add(Entry);
        }
    }

    TArray<FDTTDispatchStation> RebuiltDispatch;
    TSet<FGuid> RebuiltFuelStops;
    if (AFGVehicleSubsystem* Vehicles = AFGVehicleSubsystem::Get(GetWorld()))
    {
        for (AFGDockingStationIdentifier* Identifier : Vehicles->GetDockingStations())
        {
            if (!IsValid(Identifier)) continue;
            FString FullName = Identifier->GetStationName().ToString();

            FString Base, GroupName; int32 Slots = 1;
            const bool bHasGroup = ParseStationTag(FullName, Base, GroupName, Slots);
            if (bHasGroup)
            {
                const FString CanonicalName = MakeTaggedName(FullName, GroupName, Slots);
                if (CanonicalName != FullName && RenameDockGuarded(Identifier, CanonicalName))
                {
                    FullName = CanonicalName;
                    UE_LOG(LogDynamicTimeTableSubsystem, Verbose,
                        TEXT("GROUP_TAGS_INHERITED Kind=Truck Group='%s' Station='%s'"),
                        *GroupName, *FullName);
                }
            }

            FString FuelBase;
            if (ParseFuelStopTag(FullName, FuelBase))
            {
                RebuiltFuelStops.Add(Identifier->GetPathNodeGUID());
                UE_LOG(LogDynamicTimeTableSubsystem, Verbose,
                    TEXT("DISPATCH_FUEL_TAG_FOUND Name=\"%s\" Guid=%s"),
                    *FullName, *Identifier->GetPathNodeGUID().ToString());
            }

            if (bHasGroup)
            {
                FDTTStationEntry Entry;
                Entry.DisplayName = FullName;
                Entry.TargetSlots = Slots;
                Entry.DockingStationIdentifier = Identifier;
                Entry.DockingPathNodeGuid = Identifier->GetPathNodeGUID();
                Bucket(EDTTGroupKind::Truck, GroupName).Stations.Add(Entry);
            }

            // Independent tag, different bracket family - a dock can carry both, either, or neither.
            FString DispatchBase, ResourceId; EDTTDispatchRole Role; int32 Threshold;
            if (ParseDispatchTag(FullName, DispatchBase, ResourceId, Role, Threshold) && Role != EDTTDispatchRole::None)
            {
                FDTTDispatchStation Dispatch;
                Dispatch.DisplayName = FullName;
                Dispatch.ResourceId = ResourceId;
                Dispatch.Role = Role;
                Dispatch.Threshold = Threshold;
                Dispatch.DockingStationIdentifier = Identifier;
                Dispatch.DockingPathNodeGuid = Identifier->GetPathNodeGUID();
                RebuiltDispatch.Add(Dispatch);

                const bool bHasStation = IsValid(Identifier->GetStation());
                const bool bIsLoadMode = bHasStation && Identifier->GetStation()->GetIsInLoadMode();
                const EDTTDispatchRole EffectiveRole = GetEffectiveDispatchRole(Dispatch);
                const bool bModeMatches = DoesDispatchStationMatchMode(Dispatch);
                UE_LOG(LogDynamicTimeTableSubsystem, Verbose,
                    TEXT("DISPATCH_TAG_FOUND Name=\"%s\" Resource=\"%s\" Role=%s TagRole=%s Threshold=%d GetIsInLoadMode=%s ModeMatches=%s"),
                    *FullName, *ResourceId,
                    EffectiveRole == EDTTDispatchRole::Producer ? TEXT("Producer") : TEXT("Requester"),
                    Role == EDTTDispatchRole::ByStationMode ? TEXT("ByStationMode")
                        : (Role == EDTTDispatchRole::Producer ? TEXT("ExplicitProducer") : TEXT("ExplicitRequester")),
                    Threshold, bIsLoadMode ? TEXT("true") : TEXT("false"), bModeMatches ? TEXT("true") : TEXT("false"));
                if (!bModeMatches)
                {
                    UE_LOG(LogDynamicTimeTableSubsystem, Warning,
                        TEXT("DISPATCH_MODE_MISMATCH Name=\"%s\" TaggedRole=%s ActualMode=%s Action=ignored"),
                        *FullName, Role == EDTTDispatchRole::Producer ? TEXT("Producer") : TEXT("Requester"),
                        !bHasStation ? TEXT("Unavailable") : (bIsLoadMode ? TEXT("LOAD") : TEXT("UNLOAD")));
                }

            }
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

    // Dispatch config can change independently of the balancing groups above (e.g. only a
    // "(Iron-10)" tag was edited), so this does not wait on bChanged - RebuildGroupsFromNames
    // itself only ever runs on a name-fingerprint change, that is throttling enough.
    DispatchStations = MoveTemp(RebuiltDispatch);
    DispatchFuelStopGuids = MoveTemp(RebuiltFuelStops);
    // Probe only after the complete station set is installed. A resource id can be an alias or use
    // the client's language while a dedicated server runs in English, so resolution can fall back
    // to the sole cargo class present at the matching LOAD producer.
    for (const FDTTDispatchStation& Station : DispatchStations)
    {
        if (const TSubclassOf<UFGItemDescriptor> ItemClass = ResolveItemClassForResource(Station.ResourceId))
        {
            const int32 Stock = ReadDockStockInStacks(Station.DockingStationIdentifier.Get(), ItemClass);
            UE_LOG(LogDynamicTimeTableSubsystem, VeryVerbose,
                TEXT("DISPATCH_STOCK_PROBE Name=\"%s\" Resource=\"%s\" Item=\"%s\" StockStacks=%d Threshold=%d"),
                *Station.DisplayName, *Station.ResourceId, *UFGItemDescriptor::GetItemName(ItemClass).ToString(),
                Stock, Station.Threshold);
        }
        else
        {
            UE_LOG(LogDynamicTimeTableSubsystem, Warning,
                TEXT("DISPATCH_STOCK_PROBE Name=\"%s\" Resource=\"%s\" could not resolve an item class by name or producer cargo"),
                *Station.DisplayName, *Station.ResourceId);
        }
    }
    for (auto It = DispatchTruckAnchors.CreateIterator(); It; ++It)
    {
        if (!It.Key().IsValid() ||
            (It.Value().FuelGuid.IsValid() && !DispatchFuelStopGuids.Contains(It.Value().FuelGuid)))
        {
            It.RemoveCurrent();
            continue;
        }
        // A borrowed stop is re-decided every lap anyway; just stop treating an untagged one as fuel.
        if (It.Value().BorrowedFuelGuid.IsValid() && !DispatchFuelStopGuids.Contains(It.Value().BorrowedFuelGuid))
        {
            It.Value().BorrowedFuelGuid.Invalidate();
        }
    }
    for (auto It = RequesterClaims.CreateIterator(); It; ++It)
    {
        const bool bStillRequester = DispatchStations.ContainsByPredicate([&](const FDTTDispatchStation& S)
            { return GetEffectiveDispatchRole(S) == EDTTDispatchRole::Requester && S.DockingPathNodeGuid == It.Key(); });
        if (!bStillRequester) { RequesterClaimedAtWorldSeconds.Remove(It.Key()); It.RemoveCurrent(); }
    }

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
int32 UDynamicTimeTableSubsystem::GetDockAssignedCount(AFGDockingStationIdentifier* Id) const{
    if(!IsValid(Id))return 0;
    int32 C=0;
    for(const auto&P:TruckBookings)if(P.Key.IsValid()&&P.Value.Get()==Id)++C;
    // Dispatch trucks never use TruckBookings (they clear it every tick - "resource dispatch" in
    // ProcessCurrentTruckTarget), so a borrowed or dedicated (F) they currently hold has to be
    // counted here too, or SelectBestStation would be blind to their claims on a fuel group. An own
    // (F) counts only on laps that actually include it: the anchor outlives laps that leave it out.
    const FGuid PathNodeGuid=Id->GetPathNodeGUID();
    if(PathNodeGuid.IsValid())
        for(const auto&P:DispatchTruckAnchors)
        {
            const AFGWheeledVehicleIdentifier* Vehicle=P.Key.Get();
            if(!IsValid(Vehicle))continue;
            if(P.Value.BorrowedFuelGuid==PathNodeGuid||
               (P.Value.FuelGuid==PathNodeGuid&&Vehicle->GetVehicleRoute().Contains(PathNodeGuid)))++C;
        }
    return C;
}
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

EDTTDispatchRole UDynamicTimeTableSubsystem::GetEffectiveDispatchRole(const FDTTDispatchStation& Station) const
{
    const AFGDockingStationIdentifier* Identifier = Station.DockingStationIdentifier.Get();
    const AFGBuildableDockingStation* Dock = IsValid(Identifier) ? Identifier->GetStation() : nullptr;
    if (!IsValid(Dock)) return EDTTDispatchRole::None;
    if (Station.Role == EDTTDispatchRole::ByStationMode)
    {
        return Dock->GetIsInLoadMode() ? EDTTDispatchRole::Producer : EDTTDispatchRole::Requester;
    }
    return Station.Role;
}

bool UDynamicTimeTableSubsystem::DoesDispatchStationMatchMode(const FDTTDispatchStation& Station) const
{
    const AFGDockingStationIdentifier* Identifier = Station.DockingStationIdentifier.Get();
    const AFGBuildableDockingStation* Dock = IsValid(Identifier) ? Identifier->GetStation() : nullptr;
    if (!IsValid(Dock)) return false;
    if (Station.Role == EDTTDispatchRole::ByStationMode) return true;
    return Station.Role == EDTTDispatchRole::Producer
        ? Dock->GetIsInLoadMode()
        : Station.Role == EDTTDispatchRole::Requester && !Dock->GetIsInLoadMode();
}

FString UDynamicTimeTableSubsystem::ResolveDispatchResourceFromProducer(const FGuid& ProducerGuid) const
{
    if (!ProducerGuid.IsValid()) return FString();
    const FDTTDispatchStation* Producer = DispatchStations.FindByPredicate([&](const FDTTDispatchStation& Station)
    {
        return Station.DockingPathNodeGuid == ProducerGuid &&
            GetEffectiveDispatchRole(Station) == EDTTDispatchRole::Producer &&
            DoesDispatchStationMatchMode(Station);
    });
    return Producer ? Producer->ResourceId : FString();
}

void UDynamicTimeTableSubsystem::UpdateDispatchVehicleName(
    AFGWheeledVehicleIdentifier* Vehicle, const FString& ResourceId,
    const TArray<FGuid>& Route, int32 CurrentTargetIndex) const
{
    if (!IsValid(Vehicle) || ResourceId.IsEmpty()) return;

    FString DestinationName;
    if (Route.IsValidIndex(CurrentTargetIndex))
    {
        if (const AFGDockingStationIdentifier* Destination = FindDockByPathNode(Route[CurrentTargetIndex]))
        {
            const FString FullName = Destination->GetStationName().ToString();
            DestinationName = StripDispatchTag(FullName);
            FString WithoutFuelTag;
            if (ParseFuelStopTag(DestinationName, WithoutFuelTag)) DestinationName = WithoutFuelTag;
            DestinationName = StripStationTag(DestinationName).TrimStartAndEnd();
            if (DestinationName.IsEmpty()) DestinationName = FullName.TrimStartAndEnd();
        }
    }

    const FString WantedName = DestinationName.IsEmpty()
        ? ResourceId
        : FString::Printf(TEXT("%s → %s"), *ResourceId, *DestinationName);
    const FString PreviousName = Vehicle->GetVehicleName().ToString();
    if (PreviousName == WantedName) return;

    Vehicle->SetVehicleName(FText::FromString(WantedName));
    UE_LOG(LogDynamicTimeTableSubsystem, Verbose,
        TEXT("DISPATCH_TRUCK_RENAMED Previous='%s' New='%s' Resource='%s' Destination='%s'"),
        *PreviousName, *WantedName, *ResourceId,
        DestinationName.IsEmpty() ? TEXT("<unresolved>") : *DestinationName);
}

bool UDynamicTimeTableSubsystem::UpdateRememberedAnchors(
    AFGWheeledVehicleIdentifier* Vehicle, const FString& ResourceId, const TArray<FGuid>& Route)
{
    if (!IsValid(Vehicle) || Route.Num() < 1 || !Route[0].IsValid()) return false;

    const bool bProducerMatches = DispatchStations.ContainsByPredicate([&](const FDTTDispatchStation& Station)
    {
        return GetEffectiveDispatchRole(Station) == EDTTDispatchRole::Producer &&
            Station.ResourceId.Equals(ResourceId, ESearchCase::IgnoreCase) &&
            Station.DockingPathNodeGuid == Route[0] && DoesDispatchStationMatchMode(Station);
    });
    if (!bProducerMatches)
    {
        UE_LOG(LogDynamicTimeTableSubsystem, Verbose,
            TEXT("DISPATCH_ANCHORS_PENDING Truck='%s' Resource='%s' Reason=route-zero-is-not-load-producer"),
            *DTSVehicleName(Vehicle), *ResourceId);
        return false;
    }

    FDTTDispatchTruckAnchors* Existing = DispatchTruckAnchors.Find(Vehicle);
    if (Existing && Existing->ProducerGuid != Route[0])
    {
        // Route[0] changing is a manual base-route edit. The old fuel anchor belongs to that old
        // base route and must not leak into the new one.
        DispatchTruckAnchors.Remove(Vehicle);
        Existing = nullptr;
    }

    // The stop this mod borrowed last lap may still sit in the route; it is never the player's own.
    const FGuid Borrowed = Existing ? Existing->BorrowedFuelGuid : FGuid();
    FGuid FuelGuid;
    int32 FuelInsertIndex = INDEX_NONE;
    int32 FuelTagsInRoute = 0;
    for (int32 Index = 1; Index < Route.Num(); ++Index)
    {
        const FGuid& Candidate = Route[Index];
        if (!Candidate.IsValid() || Candidate == Borrowed || !DispatchFuelStopGuids.Contains(Candidate)) continue;
        ++FuelTagsInRoute;
        if (!FuelGuid.IsValid())
        {
            FuelGuid = Candidate;
            FuelInsertIndex = Index;
        }
    }

    if (FuelTagsInRoute > 1)
    {
        UE_LOG(LogDynamicTimeTableSubsystem, Warning,
            TEXT("DISPATCH_MULTIPLE_FUEL_TAGS Truck='%s' Resource='%s' Count=%d Action=use-first-in-route"),
            *DTSVehicleName(Vehicle), *ResourceId, FuelTagsInRoute);
    }

    if (Existing)
    {
        // Known anchors are authoritative. The route is what this mod wrote last lap, and an own (F)
        // that lap did not need is simply absent from it - absence means nothing. Only two things
        // change the anchors: an (F) they do not know about (the player just added it), and a hand
        // edit of the route, after which the (F) position is read again. A hand edit that drops the
        // (F) keeps it: the player may have edited during a lap that had left it out. To get rid of
        // an own (F), untag the station or change the first stop.
        const bool bNewFuel = FuelGuid.IsValid() && FuelGuid != Existing->FuelGuid;
        const bool bReread = FuelGuid.IsValid() && Existing->bRouteEditedByPlayer;
        Existing->bRouteEditedByPlayer = false;
        if (bNewFuel || bReread)
        {
            Existing->FuelGuid = FuelGuid;
            Existing->FuelInsertIndex = FuelInsertIndex;
            UE_LOG(LogDynamicTimeTableSubsystem, Verbose,
                TEXT("DISPATCH_ANCHORS_REMEMBERED Truck='%s' Resource='%s' Producer=%s Fuel=%s FuelInsertIndex=%d DedicatedFuel=true Reason=%s"),
                *DTSVehicleName(Vehicle), *ResourceId, *Existing->ProducerGuid.ToString(),
                *Existing->FuelGuid.ToString(), Existing->FuelInsertIndex,
                bNewFuel ? TEXT("fuel-added-to-route") : TEXT("route-edited"));
        }
        return true;
    }

    // First sighting (or a save from before 1.0.7, which stored no anchors): the route is all there
    // is, and the first (F) in it is the truck's own.
    FDTTDispatchTruckAnchors Anchors;
    Anchors.ProducerGuid = Route[0];
    Anchors.FuelGuid = FuelGuid;
    Anchors.FuelInsertIndex = FuelGuid.IsValid() ? FuelInsertIndex : INDEX_NONE;
    DispatchTruckAnchors.Add(Vehicle, Anchors);
    UE_LOG(LogDynamicTimeTableSubsystem, Verbose,
        TEXT("DISPATCH_ANCHORS_REMEMBERED Truck='%s' Resource='%s' Producer=%s Fuel=%s FuelInsertIndex=%d DedicatedFuel=%s Reason=first-sighting"),
        *DTSVehicleName(Vehicle), *ResourceId, *Anchors.ProducerGuid.ToString(),
        *Anchors.FuelGuid.ToString(), Anchors.FuelInsertIndex,
        Anchors.FuelGuid.IsValid() ? TEXT("true") : TEXT("false"));
    return true;
}

bool UDynamicTimeTableSubsystem::ReplaceRoute(
    AFGWheeledVehicleIdentifier* Vehicle, const TArray<FGuid>& NewRoute)
{
    if (!IsValid(Vehicle) || NewRoute.Num() < 2) return false;
    for (const FGuid& Node : NewRoute) if (!Node.IsValid()) return false;
    if (Vehicle->GetVehicleRoute() == NewRoute) return true;

    InternalRouteChanges.Add(Vehicle);
    Vehicle->SetVehicleRoute(NewRoute);
    InternalRouteChanges.Remove(Vehicle);
    return Vehicle->GetVehicleRoute() == NewRoute;
}

float UDynamicTimeTableSubsystem::SumSegmentFuelConsumption(
    AFGVehicleSubsystem* VehicleSubsystem,
    const FGuid& FromNodeGuid,
    const FGuid& ToNodeGuid,
    TSubclassOf<AFGWheeledVehicle> VehicleType) const
{
    if (!IsValid(VehicleSubsystem) || !FromNodeGuid.IsValid() || !ToNodeGuid.IsValid() || !VehicleType)
    {
        return -1.0f;
    }

    UFGVehiclePathNetwork* Network = VehicleSubsystem->FindNetworkByPathNodeGuid(FromNodeGuid);
    if (!IsValid(Network) || Network != VehicleSubsystem->FindNetworkByPathNodeGuid(ToNodeGuid))
    {
        return -1.0f;
    }

    const int32 FromNodeIndex = Network->FindPathNodeIndexByGuid(FromNodeGuid);
    const int32 ToNodeIndex = Network->FindPathNodeIndexByGuid(ToNodeGuid);
    if (FromNodeIndex == INDEX_NONE || ToNodeIndex == INDEX_NONE) return -1.0f;

    const int32 SegmentIndex = Network->FindPathIndexBetweenPathNodes(FromNodeIndex, ToNodeIndex);
    if (SegmentIndex == INDEX_NONE) return -1.0f;

    const FVehiclePathNetworkSegmentData Segment = Network->GetPathSegmentAtIndex(SegmentIndex);
    const float Consumption = Network->GetVehicleFuelConsumptionForSegment(Segment, VehicleType);
    return FMath::IsFinite(Consumption) && Consumption >= 0.0f ? Consumption : -1.0f;
}

float UDynamicTimeTableSubsystem::EstimatePathFuelConsumptionMW(
    AFGVehicleSubsystem* VehicleSubsystem, const UFGVehiclePathPreset* Preset,
    TSubclassOf<AFGWheeledVehicle> VehicleType, const FGuid& From, const FGuid& To) const
{
    if (!IsValid(VehicleSubsystem) || !Preset || !VehicleType || !From.IsValid() || !To.IsValid() || From == To)
    {
        return -1.0f;
    }

    UFGVehiclePathNetwork* Network = VehicleSubsystem->FindNetworkByPathNodeGuid(From);
    if (!IsValid(Network) || Network != VehicleSubsystem->FindNetworkByPathNodeGuid(To)) return -1.0f;

    TArray<FGuid> PathNodes;
    if (!Network->FindVehiclePath(From, To, Preset, PathNodes) || PathNodes.Num() == 0) return -1.0f;

    // FindVehiclePath excludes the source docking GUID and includes the destination. Prepend
    // From explicitly; otherwise the first physical path segment of every leg is omitted.
    FGuid Previous = From;
    float Total = 0.0f;
    for (const FGuid& Node : PathNodes)
    {
        if (!Node.IsValid()) return -1.0f;
        if (Node == Previous) continue;
        const float SegmentConsumption = SumSegmentFuelConsumption(VehicleSubsystem, Previous, Node, VehicleType);
        if (SegmentConsumption < 0.0f) return -1.0f;
        Total += SegmentConsumption;
        Previous = Node;
    }
    if (Previous != To) return -1.0f;

    return FMath::IsFinite(Total) && Total >= 0.0f ? Total : -1.0f;
}

float UDynamicTimeTableSubsystem::EstimateLoopFuelConsumptionMW(
    AFGWheeledVehicle* Vehicle, const TArray<FGuid>& Loop) const
{
    // A route containing only Producer cannot omit Fuel: the resulting one-stop vehicle route
    // would be invalid, and there is no delivery leg whose fuel requirement could be estimated.
    if (!IsValid(Vehicle) || Loop.Num() < 2) return -1.0f;

    const UFGVehiclePathPreset* Preset = Vehicle->GetVehiclePathPreset();
    AFGVehicleSubsystem* VehicleSubsystem = AFGVehicleSubsystem::Get(Vehicle->GetWorld());
    if (!Preset || !IsValid(VehicleSubsystem)) return -1.0f;

    float Total = 0.0f;
    for (int32 LegIndex = 0; LegIndex < Loop.Num(); ++LegIndex)
    {
        const FGuid& From = Loop[LegIndex];
        const FGuid& To = Loop[(LegIndex + 1) % Loop.Num()];
        const float LegCost = EstimatePathFuelConsumptionMW(VehicleSubsystem, Preset, Vehicle->GetClass(), From, To);
        if (LegCost < 0.0f) return -1.0f;
        Total += LegCost;
    }

    return FMath::IsFinite(Total) && Total > 0.0f ? Total : -1.0f;
}

bool UDynamicTimeTableSubsystem::TryEstimateFuelMargin(
    AFGWheeledVehicleIdentifier* Vehicle, const TArray<FGuid>& LoopWithoutFuel,
    float& OutCurrentFuelItems, float& OutPerLapItems) const
{
    if (!IsValid(Vehicle) || LoopWithoutFuel.Num() < 2) return false;

    AFGWheeledVehicle* WheeledVehicle = Vehicle->GetOwnerVehicle();
    const TSubclassOf<UFGItemDescriptor> FuelClass = Vehicle->GetFuelTypeDescriptor();
    if (!IsValid(WheeledVehicle) || !FuelClass) return false;

    UFGInventoryComponent* FuelInventory = WheeledVehicle->GetFuelInventory();
    const float EnergyPerItem = UFGItemDescriptor::GetEnergyValue(FuelClass);
    const float PerLapMW = EstimateLoopFuelConsumptionMW(WheeledVehicle, LoopWithoutFuel);
    if (!IsValid(FuelInventory) || PerLapMW <= 0.0f || !FMath::IsFinite(EnergyPerItem) || EnergyPerItem <= 0.0f)
    {
        UE_LOG(LogDynamicTimeTableSubsystem, Verbose,
            TEXT("DISPATCH_FUEL_KEEP Truck='%s' Reason=estimate-unavailable LoopStops=%d RouteCost=%.3f EnergyPerItem=%.3f"),
            *DTSVehicleName(Vehicle), LoopWithoutFuel.Num(), PerLapMW, EnergyPerItem);
        return false;
    }

    OutPerLapItems = PerLapMW / EnergyPerItem;
    OutCurrentFuelItems = static_cast<float>(FuelInventory->GetNumItems(FuelClass));
    return true;
}

bool UDynamicTimeTableSubsystem::ShouldSkipFuelStop(
    AFGWheeledVehicleIdentifier* Vehicle, const TArray<FGuid>& LoopWithoutFuel) const
{
    float CurrentFuelItems, PerLapItems;
    if (!TryEstimateFuelMargin(Vehicle, LoopWithoutFuel, CurrentFuelItems, PerLapItems)) return false;

    const float RequiredToSkip = FuelSkipMarginLaps * PerLapItems;
    const bool bSkip = CurrentFuelItems > RequiredToSkip;
    UE_LOG(LogDynamicTimeTableSubsystem, Verbose,
        TEXT("DISPATCH_FUEL_EVALUATED Truck='%s' LoopStops=%d PerLapItems=%.3f CurrentFuelItems=%.0f RequiredToSkip=%.3f Skip=%s"),
        *DTSVehicleName(Vehicle), LoopWithoutFuel.Num(), PerLapItems, CurrentFuelItems, RequiredToSkip,
        bSkip ? TEXT("true") : TEXT("false"));
    return bSkip;
}

bool UDynamicTimeTableSubsystem::IsFuelCriticallyLow(
    AFGWheeledVehicleIdentifier* Vehicle, const TArray<FGuid>& LoopWithoutFuel) const
{
    float CurrentFuelItems, PerLapItems;
    if (!TryEstimateFuelMargin(Vehicle, LoopWithoutFuel, CurrentFuelItems, PerLapItems)) return false;

    // Deliberately below ShouldSkipFuelStop's FuelSkipMarginLaps comfort margin: a route with no (F)
    // of its own is an intentional choice (docs: "assumed to refuel at regular stations"), so borrowing
    // shouldn't kick in as early as that buffer - but it still needs real margin (not exactly 1x),
    // because the engine's per-segment fuel estimate is unverified against actual consumption, and a
    // borrowed stop lengthens the lap beyond what PerLapItems checked (computed before insertion).
    // A bare 1x threshold let a truck read as "fine" one lap and reach exactly zero fuel the next.
    const float RequiredWithMargin = FuelCriticalMarginLaps * PerLapItems;
    const bool bLow = CurrentFuelItems < RequiredWithMargin;
    UE_LOG(LogDynamicTimeTableSubsystem, Verbose,
        TEXT("DISPATCH_FUEL_LOW_CHECK Truck='%s' LoopStops=%d PerLapItems=%.3f CurrentFuelItems=%.0f RequiredWithMargin=%.3f Low=%s"),
        *DTSVehicleName(Vehicle), LoopWithoutFuel.Num(), PerLapItems, CurrentFuelItems, RequiredWithMargin,
        bLow ? TEXT("true") : TEXT("false"));
    return bLow;
}

FGuid UDynamicTimeTableSubsystem::FindNearestFuelStop(AFGWheeledVehicle* Vehicle, const FGuid& FromGuid) const
{
    if (!IsValid(Vehicle) || !FromGuid.IsValid() || DispatchFuelStopGuids.Num() == 0) return FGuid();

    AFGVehicleSubsystem* VehicleSubsystem = AFGVehicleSubsystem::Get(Vehicle->GetWorld());
    const UFGVehiclePathPreset* Preset = Vehicle->GetVehiclePathPreset();
    if (!IsValid(VehicleSubsystem) || !Preset) return FGuid();

    FGuid Nearest;
    float BestCost = TNumericLimits<float>::Max();
    for (const FGuid& Candidate : DispatchFuelStopGuids)
    {
        if (!Candidate.IsValid() || Candidate == FromGuid) continue;
        const float Cost = EstimatePathFuelConsumptionMW(VehicleSubsystem, Preset, Vehicle->GetClass(), FromGuid, Candidate);
        if (Cost < 0.0f) continue;
        if (Cost < BestCost) { BestCost = Cost; Nearest = Candidate; }
    }
    return Nearest;
}

TArray<FGuid> UDynamicTimeTableSubsystem::SelectRequestersForResource(
    const FString& ResourceId, AFGWheeledVehicleIdentifier* Vehicle)
{
    TArray<FGuid> Result;
    if (!IsValid(Vehicle)) return Result;

    const TSubclassOf<UFGItemDescriptor> ItemClass = ResolveItemClassForResource(ResourceId);
    if (!ItemClass)
    {
        UE_LOG(LogDynamicTimeTableSubsystem, Warning,
            TEXT("DISPATCH_REQUESTER_SKIPPED Truck='%s' Resource='%s' Reason=item-class-not-found"),
            *DTSVehicleName(Vehicle), *ResourceId);
        return Result;
    }

    struct FDispatchCandidate
    {
        const FDTTDispatchStation* Station = nullptr;
        int32 StockStacks = 0;
        double FillRatio = 0.0;
    };
    TArray<FDispatchCandidate> Candidates;
    for (const FDTTDispatchStation& Station : DispatchStations)
    {
        if (GetEffectiveDispatchRole(Station) != EDTTDispatchRole::Requester ||
            !Station.ResourceId.Equals(ResourceId, ESearchCase::IgnoreCase) ||
            !DoesDispatchStationMatchMode(Station) ||
            !IsValid(Station.DockingStationIdentifier.Get()) ||
            !IsValid(Station.DockingStationIdentifier->GetStation()) ||
            !Station.DockingPathNodeGuid.IsValid())
        {
            continue;
        }

        if (const TWeakObjectPtr<AFGWheeledVehicleIdentifier>* Claim = RequesterClaims.Find(Station.DockingPathNodeGuid))
        {
            if (Claim->IsValid() && Claim->Get() != Vehicle) continue;
        }

        const int32 StockStacks = ReadDockStockInStacks(Station.DockingStationIdentifier.Get(), ItemClass);
        UE_LOG(LogDynamicTimeTableSubsystem, VeryVerbose,
            TEXT("DISPATCH_CANDIDATE Truck='%s' Resource='%s' Station='%s' StockStacks=%d Threshold=%d Open=%s"),
            *DTSVehicleName(Vehicle), *ResourceId, *Station.DisplayName, StockStacks, Station.Threshold,
            StockStacks < Station.Threshold ? TEXT("true") : TEXT("false"));
        if (StockStacks < Station.Threshold)
        {
            FDispatchCandidate Candidate;
            Candidate.Station = &Station;
            Candidate.StockStacks = StockStacks;
            Candidate.FillRatio = Station.Threshold > 0
                ? static_cast<double>(StockStacks) / static_cast<double>(Station.Threshold)
                : 1.0;
            Candidates.Add(Candidate);
        }
    }

    if (Candidates.Num() == 0) return Result;
    // Lowest stock/threshold ratio is the most starved requester. Name and GUID make ties stable
    // across subsystem rebuilds and server restarts.
    Candidates.Sort([](const FDispatchCandidate& A, const FDispatchCandidate& B)
    {
        if (!FMath::IsNearlyEqual(A.FillRatio, B.FillRatio)) return A.FillRatio < B.FillRatio;
        if (A.Station->DisplayName != B.Station->DisplayName)
            return A.Station->DisplayName < B.Station->DisplayName;
        return A.Station->DockingPathNodeGuid.ToString() < B.Station->DockingPathNodeGuid.ToString();
    });

    const UWorld* World = GetWorld();
    const double ClaimedAt = IsValid(World) ? World->GetTimeSeconds() : 0.0;
    const FDispatchCandidate& Candidate = Candidates[0];
    const FDTTDispatchStation* Selected = Candidate.Station;
    RequesterClaims.Add(Selected->DockingPathNodeGuid, Vehicle);
    RequesterClaimedAtWorldSeconds.Add(Selected->DockingPathNodeGuid, ClaimedAt);
    Result.Add(Selected->DockingPathNodeGuid);
    UE_LOG(LogDynamicTimeTableSubsystem, Verbose,
        TEXT("DISPATCH_CLAIMED Truck='%s' Resource='%s' Requester='%s' Guid=%s Priority=0 StockStacks=%d Threshold=%d FillRatio=%.6f"),
        *DTSVehicleName(Vehicle), *ResourceId, *Selected->DisplayName,
        *Selected->DockingPathNodeGuid.ToString(), Candidate.StockStacks,
        Selected->Threshold, Candidate.FillRatio);
    return Result;
}

void UDynamicTimeTableSubsystem::ProcessDispatchTruckTarget(
    AFGWheeledVehicleIdentifier* Vehicle, const FString& ResourceId, const TCHAR* Trigger)
{
    if (!IsValid(Vehicle) || !Vehicle->IsAutopilotEnabled()) return;
    const TArray<FGuid> Route = Vehicle->GetVehicleRoute();
    const int32 Index = Vehicle->GetCurrentTargetWaypointIndex();
    // The whole lap is decided here, at the producer boundary, and written with one SetVehicleRoute.
    // Nothing happens mid-lap. 1.0.1-1.0.6 skipped an unneeded (F) in place by calling
    // SetCurrentTargetWaypoint, but that is the autopilot's own status setter, not a command: it
    // moved the reported index without bumping the route changelist, and PollTruckStates then
    // recorded the moved index, so the wrap to 0 was never seen and no boundary ran again - the
    // truck stopped refuelling for good and circled whatever the route last held.
    if (Index != 0 || Route.Num() < 1) return;

    if (!UpdateRememberedAnchors(Vehicle, ResourceId, Route)) return;

    FDTTDispatchTruckAnchors* Anchors = DispatchTruckAnchors.Find(Vehicle);
    if (!Anchors || !Anchors->ProducerGuid.IsValid()) return;

    // Index zero starts a new lap. A docking transition normally released the previous claim;
    // this also closes the gap if that transition was missed while the actor was not observable.
    ReleaseRequesterClaimsForVehicle(Vehicle, TEXT("producer boundary"));
    const TArray<FGuid> Requesters = SelectRequestersForResource(ResourceId, Vehicle);

    // The loop this lap delivers on. It never contains a fuel stop, so the fuel estimate is always
    // taken on it directly. With no demand the truck keeps its last delivery loop - Producer and the
    // last consumer - minus whichever (F) last lap added.
    TArray<FGuid> BaseLoop;
    BaseLoop.Add(Anchors->ProducerGuid);
    if (Requesters.Num() > 0)
    {
        BaseLoop.Append(Requesters);
    }
    else
    {
        for (int32 RouteIndex = 1; RouteIndex < Route.Num(); ++RouteIndex)
        {
            const FGuid& Node = Route[RouteIndex];
            if (Node == Anchors->FuelGuid || Node == Anchors->BorrowedFuelGuid) continue;
            BaseLoop.Add(Node);
        }
    }
    const TCHAR* BaseSource = Requesters.Num() > 0 ? TEXT("requester") : TEXT("kept-last-loop");

    if (BaseLoop.Num() < 2)
    {
        // [Producer] alone, or [Producer, (F)]: no delivery leg to estimate fuel on, and a one-stop
        // route cannot be written. Leave the route exactly as it is. A one-stop route never changes
        // its target index, so PollTruckStates re-checks it on a timer until the first request opens.
        UE_LOG(LogDynamicTimeTableSubsystem, Verbose,
            TEXT("DISPATCH_ROUTE_KEPT Truck='%s' Resource='%s' Reason=no-delivery-loop RouteStops=%d Trigger=%s"),
            *DTSVehicleName(Vehicle), *ResourceId, Route.Num(), Trigger);
        return;
    }

    TArray<FGuid> NewRoute = BaseLoop;
    const bool bHasDedicatedFuel = Anchors->FuelGuid.IsValid();
    const FGuid PreviousBorrowed = Anchors->BorrowedFuelGuid;
    // Cleared before any selection so this truck's own stale borrow does not count against the
    // fuel group it is about to choose from (GetDockAssignedCount).
    Anchors->BorrowedFuelGuid.Invalidate();
    bool bFuelIncluded = false;

    if (bHasDedicatedFuel)
    {
        // A player's own (F). In the lap only when the tank needs it; the anchor keeps it, and its
        // position, for the laps that leave it out. If the same dock is this lap's requester it is
        // visited anyway - a delivery is never dropped just because the dock also carries (F).
        if (BaseLoop.Contains(Anchors->FuelGuid))
        {
            bFuelIncluded = true;
        }
        else if (!ShouldSkipFuelStop(Vehicle, BaseLoop))
        {
            NewRoute.Insert(Anchors->FuelGuid, FMath::Clamp(Anchors->FuelInsertIndex, 1, NewRoute.Num()));
            bFuelIncluded = true;
        }
    }
    else
    {
        // No (F) of this truck's own. Normally that is a deliberate choice (docs: assumed to refuel
        // at regular stations) - but if the tank is genuinely at risk this lap, borrow the nearest
        // reachable (F) instead of letting the truck strand. Decided afresh every lap: a borrowed
        // stop is dropped again as soon as the tank no longer needs it.
        const bool bIsCriticallyLow = IsFuelCriticallyLow(Vehicle, BaseLoop);
        FGuid ChosenGuid;
        if (bIsCriticallyLow)
        {
            ChosenGuid = FindNearestFuelStop(Vehicle->GetOwnerVehicle(), Anchors->ProducerGuid);
            if (ChosenGuid.IsValid())
            {
                // If the nearest (F) also carries its own [tag,N] balancing tag, it is a member of a
                // real station group - reuse the existing least-loaded selection instead of always
                // sending every low-fuel truck to the single physically nearest dock.
                if (FDTTStationGroup* Group = FindGroupByDock(FindDockByPathNode(ChosenGuid)))
                {
                    if (FDTTStationEntry* Best = SelectBestStation(*Group))
                    {
                        if (Best->DockingPathNodeGuid.IsValid()) ChosenGuid = Best->DockingPathNodeGuid;
                    }
                }
            }
        }

        if (ChosenGuid.IsValid())
        {
            Anchors->BorrowedFuelGuid = ChosenGuid;
            // Right after the producer, not at the tail. The trigger fires with as little as one
            // estimated lap in the tank, and a tail stop would demand the whole delivery lap plus
            // the detour on that fuel. FindNearestFuelStop measured from the producer, so this is
            // also the stop's shortest leg.
            if (!NewRoute.Contains(ChosenGuid)) NewRoute.Insert(ChosenGuid, 1);
            bFuelIncluded = true;
            UE_LOG(LogDynamicTimeTableSubsystem, Display,
                TEXT("DISPATCH_FUEL_BORROWED Truck='%s' Resource='%s' Fuel=%s Trigger=%s"),
                *DTSVehicleName(Vehicle), *ResourceId, *ChosenGuid.ToString(), Trigger);
        }
        else if (bIsCriticallyLow)
        {
            // Flagged low, but nothing to borrow at all - no (F) is tagged, or none is reachable on
            // this path network. MyDTT cannot fix this automatically; at minimum make it visible
            // instead of the truck silently running dry with no trail in the log.
            UE_LOG(LogDynamicTimeTableSubsystem, Warning,
                TEXT("DISPATCH_FUEL_NO_STOP_FOUND Truck='%s' Resource='%s' Trigger=%s"),
                *DTSVehicleName(Vehicle), *ResourceId, Trigger);
        }
    }

    if (NewRoute == Route) return;
    if (!ReplaceRoute(Vehicle, NewRoute))
    {
        // Selection claims before the write so two trucks cannot select in the same poll. Roll it
        // back if the route mutation failed; otherwise the requester would be blocked for 10 min.
        for (const FGuid& Requester : Requesters)
        {
            const TWeakObjectPtr<AFGWheeledVehicleIdentifier>* Owner = RequesterClaims.Find(Requester);
            if (Owner && Owner->Get() == Vehicle)
            {
                RequesterClaims.Remove(Requester);
                RequesterClaimedAtWorldSeconds.Remove(Requester);
            }
        }
        // The old route still holds the old borrowed stop; forgetting it would let the next
        // boundary mistake it for an (F) the player put there.
        Anchors->BorrowedFuelGuid = PreviousBorrowed;
        UE_LOG(LogDynamicTimeTableSubsystem, Warning,
            TEXT("DISPATCH_ROUTE_FAILED Truck='%s' Resource='%s' Trigger=%s"),
            *DTSVehicleName(Vehicle), *ResourceId, Trigger);
        return;
    }

    UE_LOG(LogDynamicTimeTableSubsystem, Verbose,
        TEXT("DISPATCH_ROUTE_REPLACED Truck='%s' Resource='%s' Requesters=%d Base=%s DedicatedFuel=%s FuelBorrowed=%s FuelIncluded=%s FuelInsertIndex=%d Stops=%d Trigger=%s"),
        *DTSVehicleName(Vehicle), *ResourceId, Requesters.Num(), BaseSource,
        bHasDedicatedFuel ? TEXT("true") : TEXT("false"),
        Anchors->BorrowedFuelGuid.IsValid() ? TEXT("true") : TEXT("false"),
        bFuelIncluded ? TEXT("true") : TEXT("false"), Anchors->FuelInsertIndex, NewRoute.Num(), Trigger);
}

void UDynamicTimeTableSubsystem::ReleaseRequesterClaimIfDocked(AFGWheeledVehicleIdentifier* Vehicle)
{
    if (!IsValid(Vehicle)) return;
    const TArray<FGuid>& Route = Vehicle->GetVehicleRoute();
    const int32 Index = Vehicle->GetCurrentTargetWaypointIndex();
    if (!Route.IsValidIndex(Index)) return;

    const FGuid DockGuid = Route[Index];
    const TWeakObjectPtr<AFGWheeledVehicleIdentifier>* Owner = RequesterClaims.Find(DockGuid);
    if (!Owner || Owner->Get() != Vehicle) return;

    RequesterClaims.Remove(DockGuid);
    RequesterClaimedAtWorldSeconds.Remove(DockGuid);
    UE_LOG(LogDynamicTimeTableSubsystem, Verbose,
        TEXT("DISPATCH_CLAIM_RELEASED Truck='%s' RequesterGuid=%s Reason=truck-docked"),
        *DTSVehicleName(Vehicle), *DockGuid.ToString());
}

void UDynamicTimeTableSubsystem::ReleaseRequesterClaimsForVehicle(
    AFGWheeledVehicleIdentifier* Vehicle, const TCHAR* Reason)
{
    if (!IsValid(Vehicle)) return;
    for (auto It = RequesterClaims.CreateIterator(); It; ++It)
    {
        if (It.Value().Get() != Vehicle) continue;
        const FGuid RequesterGuid = It.Key();
        RequesterClaimedAtWorldSeconds.Remove(RequesterGuid);
        It.RemoveCurrent();
        UE_LOG(LogDynamicTimeTableSubsystem, Verbose,
            TEXT("DISPATCH_CLAIM_RELEASED Truck='%s' RequesterGuid=%s Reason=%s"),
            *DTSVehicleName(Vehicle), *RequesterGuid.ToString(), Reason);
    }
}

void UDynamicTimeTableSubsystem::PruneStaleRequesterClaims()
{
    const UWorld* World = GetWorld();
    const double Now = IsValid(World) ? World->GetTimeSeconds() : 0.0;
    for (auto It = RequesterClaims.CreateIterator(); It; ++It)
    {
        const double* ClaimedAt = RequesterClaimedAtWorldSeconds.Find(It.Key());
        const bool bTimedOut = !ClaimedAt || Now - *ClaimedAt > RequesterClaimTimeoutSeconds;
        const FDTTDispatchStation* Requester = DispatchStations.FindByPredicate([&](const FDTTDispatchStation& Station)
        {
            return GetEffectiveDispatchRole(Station) == EDTTDispatchRole::Requester && Station.DockingPathNodeGuid == It.Key();
        });
        const bool bRequesterActive = Requester && DoesDispatchStationMatchMode(*Requester);
        if (It.Value().IsValid() && !bTimedOut && bRequesterActive) continue;

        const FGuid RequesterGuid = It.Key();
        const FString TruckName = It.Value().IsValid() ? DTSVehicleName(It.Value().Get()) : TEXT("<destroyed>");
        RequesterClaimedAtWorldSeconds.Remove(RequesterGuid);
        It.RemoveCurrent();
        UE_LOG(LogDynamicTimeTableSubsystem, Verbose,
            TEXT("DISPATCH_CLAIM_PRUNED Truck='%s' RequesterGuid=%s Reason=%s"),
            *TruckName, *RequesterGuid.ToString(), bTimedOut ? TEXT("timeout") :
                (!bRequesterActive ? TEXT("requester-not-in-unload-mode") : TEXT("vehicle-invalid")));
    }

    for (auto It = RequesterClaimedAtWorldSeconds.CreateIterator(); It; ++It)
        if (!RequesterClaims.Contains(It.Key())) It.RemoveCurrent();
    for (auto It = DispatchTruckAnchors.CreateIterator(); It; ++It)
        if (!It.Key().IsValid()) It.RemoveCurrent();
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
    PruneStaleRequesterClaims();

    const TArray<FGuid>& Route = Vehicle->GetVehicleRoute();
    const int32 Index = Vehicle->GetCurrentTargetWaypointIndex();
    const FString ResourceId = Route.Num() > 0
        ? ResolveDispatchResourceFromProducer(Route[0]) : FString();
    if (!ResourceId.IsEmpty())
    {
        // Dispatch and load-balancing are independent modes. Route[0], not the mutable display
        // name, opts this truck in and supplies the resource for every subsequent waypoint.
        UpdateDispatchVehicleName(Vehicle, ResourceId, Route, Index);
        ReleaseTruckBooking(Vehicle, TEXT("resource dispatch"));
        ProcessDispatchTruckTarget(Vehicle, ResourceId, Trigger);
        return;
    }

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
    PruneStaleRequesterClaims();

    for (auto It = TruckPollStates.CreateIterator(); It; ++It)
        if (!It.Key().IsValid()) It.RemoveCurrent();

    for (AFGWheeledVehicleIdentifier* Vehicle : V->GetAllVehicles())
    {
        if (!IsValid(Vehicle)) continue;
        if (!Vehicle->IsAutopilotEnabled())
        {
            // Claims go, anchors stay: an own (F) that the current lap left out of the route exists
            // only in the anchors, and switching the autopilot off and on must not forget it.
            ReleaseRequesterClaimsForVehicle(Vehicle, TEXT("autopilot disabled"));
            TruckPollStates.Remove(Vehicle);
            continue;
        }

        const int32 TargetIndex = Vehicle->GetCurrentTargetWaypointIndex();
        const bool bDocking = Vehicle->IsCurrentlyDocking();
        const TArray<FGuid>& Route = Vehicle->GetVehicleRoute();
        const FString ResourceId = Route.Num() > 0
            ? ResolveDispatchResourceFromProducer(Route[0]) : FString();
        if (!ResourceId.IsEmpty())
            UpdateDispatchVehicleName(Vehicle, ResourceId, Route, TargetIndex);
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
            ReleaseRequesterClaimIfDocked(Vehicle);
        }

        // The autopilot moving on to the next waypoint is the moment to substitute.
        if (TargetIndex != Previous->TargetIndex)
        {
            ProcessCurrentTruckTarget(Vehicle, TEXT("waypoint changed"));
        }
        else if (!ResourceId.IsEmpty() && Route.Num() < 2)
        {
            // A dispatch truck parked on a one-stop [Producer] route: its target index never
            // changes, so the branch above would never fire and the first request to open would go
            // unnoticed for good. Re-evaluate it on a slow timer instead.
            const UWorld* World = GetWorld();
            const double Now = IsValid(World) ? World->GetTimeSeconds() : 0.0;
            if (Now - Previous->LastIdleRecheckSeconds >= IdleDispatchRecheckSeconds)
            {
                Previous->LastIdleRecheckSeconds = Now;
                ProcessCurrentTruckTarget(Vehicle, TEXT("idle recheck"));
            }
        }

        // The index observed before the call, on purpose: a boundary is recognised only by seeing
        // the index change, so recording anything else here can hide the next one.
        Previous->TargetIndex = TargetIndex;
        Previous->bDocking = bDocking;
    }
}

void UDynamicTimeTableSubsystem::HandleVehicleRouteChanged(AFGWheeledVehicleIdentifier* Vehicle)
{
    if (!IsServer() || !IsValid(Vehicle) || InternalRouteChanges.Contains(Vehicle)) return;
    ReleaseTruckBooking(Vehicle, TEXT("route saved"));
    ReleaseRequesterClaimsForVehicle(Vehicle, TEXT("route saved"));
    // Not removed: the player may have edited during a lap that left the own (F) out, and the route
    // they saved then does not show it. The next boundary re-reads the (F) from the route if there
    // is one, and drops everything if the first stop changed (UpdateRememberedAnchors).
    if (FDTTDispatchTruckAnchors* Anchors = DispatchTruckAnchors.Find(Vehicle))
    {
        Anchors->bRouteEditedByPlayer = true;
    }
    PendingRouteReevaluations.Add(Vehicle);
    SchedulePendingRouteReevaluation();
    UE_LOG(LogDynamicTimeTableSubsystem, Display, TEXT("ROUTE_CHANGED Truck='%s' Waypoints=%d AutoPilot=%s BookingCleared=true Reevaluation=next-tick"),
        *DTSVehicleName(Vehicle), Vehicle->GetVehicleRoute().Num(), Vehicle->IsAutopilotEnabled() ? TEXT("true") : TEXT("false"));
}

void UDynamicTimeTableSubsystem::HandleAutopilotChanged(AFGWheeledVehicleIdentifier* Vehicle, bool bEnabled)
{
    if (!IsServer() || !IsValid(Vehicle)) return;
    ReleaseTruckBooking(Vehicle, bEnabled ? TEXT("autopilot enabled") : TEXT("autopilot disabled"));
    ReleaseRequesterClaimsForVehicle(Vehicle, bEnabled ? TEXT("autopilot enabled") : TEXT("autopilot disabled"));
    // Anchors survive the toggle for the same reason as in PollTruckStates.
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

TSubclassOf<UFGItemDescriptor> UDynamicTimeTableSubsystem::ResolveItemClassByName(const FString& Name) const
{
    const FString Key = Name.ToLower();
    if (const auto* Found = ItemClassByNameCache.Find(Key)) return *Found;

    // Populated once, not per lookup: GetAllDescriptorsSorted walks every item descriptor in the game.
    if (ItemClassByNameCache.Num() == 0)
    {
        TArray<TSubclassOf<UFGItemDescriptor>> All;
        UFGBlueprintFunctionLibrary::GetAllDescriptorsSorted(const_cast<UDynamicTimeTableSubsystem*>(this), All,
            /*includeFluids*/ false, /*includeBuildings*/ false, /*onlyUnlocked*/ false);
        for (const TSubclassOf<UFGItemDescriptor>& Class : All)
        {
            if (!Class) continue;
            ItemClassByNameCache.Add(UFGItemDescriptor::GetItemName(Class).ToString().ToLower(), Class);
        }
    }

    const auto* Found = ItemClassByNameCache.Find(Key);
    return Found ? *Found : nullptr;
}

TSubclassOf<UFGItemDescriptor> UDynamicTimeTableSubsystem::ResolveItemClassForResource(const FString& ResourceId) const
{
    if (const TSubclassOf<UFGItemDescriptor> ByName = ResolveItemClassByName(ResourceId)) return ByName;

    TSet<TSubclassOf<UFGItemDescriptor>> ProducerCargoClasses;
    for (const FDTTDispatchStation& Station : DispatchStations)
    {
        if (GetEffectiveDispatchRole(Station) != EDTTDispatchRole::Producer ||
            !Station.ResourceId.Equals(ResourceId, ESearchCase::IgnoreCase) ||
            !DoesDispatchStationMatchMode(Station))
        {
            continue;
        }

        AFGDockingStationIdentifier* Identifier = Station.DockingStationIdentifier.Get();
        AFGBuildableDockingStation* Dock = IsValid(Identifier) ? Identifier->GetStation() : nullptr;
        UFGInventoryComponent* Storage = IsValid(Dock) ? Dock->GetInventory() : nullptr;
        if (!IsValid(Storage)) continue;

        TArray<FInventoryStack> Stacks;
        Storage->GetInventoryStacks(Stacks);
        for (const FInventoryStack& Stack : Stacks)
        {
            if (Stack.HasItems()) ProducerCargoClasses.Add(Stack.Item.GetItemClass());
        }
    }

    if (ProducerCargoClasses.Num() != 1) return nullptr;
    return *ProducerCargoClasses.CreateConstIterator();
}

int32 UDynamicTimeTableSubsystem::ReadDockStockInStacks(AFGDockingStationIdentifier* Dock, TSubclassOf<UFGItemDescriptor> ItemClass) const
{
    if (!IsValid(Dock) || !ItemClass) return 0;
    AFGBuildableDockingStation* Building = Dock->GetStation();
    UFGInventoryComponent* Storage = IsValid(Building) ? Building->GetInventory() : nullptr;
    if (!IsValid(Storage)) return 0;
    const int32 StackSize = FMath::Max(1, UFGItemDescriptor::GetStackSize(ItemClass));
    return Storage->GetNumItems(ItemClass) / StackSize;
}
