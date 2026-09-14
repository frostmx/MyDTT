#include "DynamicTimeTableSubsystem.h"

#include "DTTSaveDataActor.h"
#include "DynamicTimeTableChatCommand.h"
#include "MyDTTHooks.h"
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
#include "FGDockableInterface.h"
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
        if (!IsValid(Vehicle)) return TEXT("<none>");
        // Two trucks on the same producer carry the same display name (it is "Resource -> stop"),
        // so the identifier's object number is appended to tell their log lines apart.
        FString Number = Vehicle->GetName();
        int32 Underscore = INDEX_NONE;
        if (Number.FindLastChar(TEXT('_'), Underscore)) Number = Number.Mid(Underscore + 1);
        return FString::Printf(TEXT("%s#%s"), *Vehicle->GetVehicleName().ToString(), *Number);
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
            // The offset is whatever follows the last '-', and only if it is a plain
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
            if (Role == EDTTDispatchRole::None && !Inner.Equals(TEXT("F"), ESearchCase::IgnoreCase) &&
                !Inner.Equals(TEXT("D"), ESearchCase::IgnoreCase))
            {
                Resource = Inner;
                if (!Resource.IsEmpty())
                {
                    Role = EDTTDispatchRole::ByStationMode;
                    // No number: the settings' limit applies (GetRequestLevels).
                    Threshold = INDEX_NONE;
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

bool UDynamicTimeTableSubsystem::ParseDepotTag(const FString& FullName, FString& OutBaseName)
{
    OutBaseName = FullName.TrimStartAndEnd();
    static const FString Tag = TEXT("(D)");
    const int32 TagIndex = FullName.Find(Tag, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
    if (TagIndex == INDEX_NONE) return false;
    OutBaseName = (FullName.Left(TagIndex) + FullName.Mid(TagIndex + Tag.Len())).TrimStartAndEnd();
    return true;
}

FString UDynamicTimeTableSubsystem::StripAllTags(const FString& FullName)
{
    FString Name = StripDispatchTag(FullName);
    FString Stripped;
    if (ParseFuelStopTag(Name, Stripped)) Name = Stripped;
    if (ParseDepotTag(Name, Stripped)) Name = Stripped;
    Name = StripStationTag(Name).TrimStartAndEnd();
    return Name.IsEmpty() ? FullName.TrimStartAndEnd() : Name;
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
    DispatchDepotGuids.Reset();
    DepotClaims.Reset();
    PoolTrucks.Reset();
    bParkedTrucksRestored = false;
    LastPoolDispatchSeconds = 0.0;
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

        // The hold on a parked truck is re-applied by RestoreParkedTrucks once the stations are
        // readable; until then the entry only says which trucks were parked and where.
        int32 PoolRestored = 0;
        for (const FDTTSavedPoolTruck& Saved : Found->GetPoolTrucks())
        {
            if (!IsValid(Saved.Vehicle.Get()) || Saved.State.Phase == EDTTTruckPhase::Unmanaged) continue;
            PoolTrucks.Add(Saved.Vehicle.Get(), Saved.State);
            ++PoolRestored;
        }
        UE_LOG(LogDynamicTimeTableSubsystem, Display,
            TEXT("POOL_RESTORED Count=%d Saved=%d"), PoolRestored, Found->GetPoolTrucks().Num());

        if (Found->GetDataVersion() >= 7)
        {
            Settings = Found->GetSettings();
            Settings.Sanitize();
            UE_LOG(LogDynamicTimeTableSubsystem, Display, TEXT("SETTINGS_LOADED %s"), *Settings.Describe());
        }
        else
        {
            UE_LOG(LogDynamicTimeTableSubsystem, Display, TEXT("SETTINGS_DEFAULT DataVersion=%d %s"), Found->GetDataVersion(), *Settings.Describe());
        }
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
    // game saves itself. It is kept because the class is referenced by existing saves; since
    // version 5 it also carries the dispatch fuel anchors (see ExportTruckAnchors) and since 6 the
    // pool trucks (ExportPoolTrucks).
    if (SaveDataActor.IsValid() && IsServer())
    {
        SaveDataActor->SetGroups(TArray<FDTTStationGroup>());
        SaveDataActor->SetDataVersion(7);
        SaveDataActor->SetSettings(Settings);
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

void UDynamicTimeTableSubsystem::ExportPoolTrucks(TArray<FDTTSavedPoolTruck>& OutTrucks) const
{
    OutTrucks.Reset();
    for (const auto& Pair : PoolTrucks)
    {
        if (!Pair.Key.IsValid() || Pair.Value.Phase == EDTTTruckPhase::Unmanaged) continue;
        FDTTSavedPoolTruck& Saved = OutTrucks.AddDefaulted_GetRef();
        Saved.Vehicle = Pair.Key.Get();
        Saved.State = Pair.Value;
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
        ? FString::Printf(TEXT("dTT dispatch: stations=%d fuel-stops=%d depots=%d active-claims=%d pool=%d"),
            DispatchStations.Num(), DispatchFuelStopGuids.Num(), DispatchDepotGuids.Num(), RequesterClaims.Num(), PoolTrucks.Num())
        : FString::Printf(TEXT("dTT dispatch: resource='%s'"), *ResourceFilter));

    // Stations, then depots and fuel stops (only without a filter: they are not tied to one
    // resource and chat has no scrollback to spare), then the trucks - one line each, the route
    // being the only place a multi-stop lap or a cut lap can be seen from chat.
    TArray<FDTTDispatchRow> Rows;
    BuildDispatchRows(Rows, ResourceFilter);
    int32 Stations = 0;
    for (const FDTTDispatchRow& Row : Rows)
    {
        if (!Row.bDepot && !Row.bFuelStop) ++Stations;
        OutLines.Add(FormatDispatchRow(Row));
    }
    TArray<FDTTTruckRow> Trucks;
    BuildTruckRows(Trucks, ResourceFilter);
    for (const FDTTTruckRow& Row : Trucks) OutLines.Add(FormatTruckRow(Row));

    if (!ResourceFilter.IsEmpty() && Stations == 0)
    {
        OutLines.Add(FString::Printf(
            TEXT("No dispatch stations tagged with resource '%s'."), *ResourceFilter));
    }
    else if (DispatchStations.Num() == 0 && DispatchFuelStopGuids.Num() == 0 && DispatchDepotGuids.Num() == 0)
    {
        OutLines.Add(TEXT("No dispatch tags. Use (Resource) on LOAD/UNLOAD stations - an UNLOAD station "
            "requests while it is not full; optional (Resource-N) requests only with more than N stacks "
            "free - optional (F) on a fuel stop, and (D) on a depot where pool trucks wait."));
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
    TSet<FGuid> RebuiltDepots;
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
            FString DepotBase;
            if (ParseDepotTag(FullName, DepotBase))
            {
                RebuiltDepots.Add(Identifier->GetPathNodeGUID());
                UE_LOG(LogDynamicTimeTableSubsystem, Verbose,
                    TEXT("DEPOT_TAG_FOUND Name=\"%s\" Guid=%s"),
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
                    TEXT("DISPATCH_TAG_FOUND Name=\"%s\" Resource=\"%s\" Role=%s TagRole=%s Offset=%d GetIsInLoadMode=%s ModeMatches=%s"),
                    *FullName, *ResourceId,
                    EffectiveRole == EDTTDispatchRole::Producer ? TEXT("Producer") : TEXT("Requester"),
                    Role == EDTTDispatchRole::ByStationMode ? TEXT("ByStationMode")
                        : (Role == EDTTDispatchRole::Producer ? TEXT("ExplicitProducer") : TEXT("ExplicitRequester")),
                    FMath::Max(0, Threshold),
                    bIsLoadMode ? TEXT("true") : TEXT("false"), bModeMatches ? TEXT("true") : TEXT("false"));
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
    DispatchDepotGuids = MoveTemp(RebuiltDepots);
    // A depot that lost its tag frees its claim; a truck parked there is let go, and one heading
    // there picks another depot at its next boundary.
    for (auto It = DepotClaims.CreateIterator(); It; ++It)
    {
        if (!It.Value().IsValid() || !DispatchDepotGuids.Contains(It.Key())) It.RemoveCurrent();
    }
    for (auto It = PoolTrucks.CreateIterator(); It; ++It)
    {
        if (!It.Key().IsValid()) { It.RemoveCurrent(); continue; }
        FDTTPoolTruckState& State = It.Value();
        if (State.DepotGuid.IsValid() && !DispatchDepotGuids.Contains(State.DepotGuid))
        {
            if (State.Phase == EDTTTruckPhase::Parked)
            {
                UnparkTruck(It.Key().Get(), State, EDTTTruckPhase::ToDepot, TEXT("depot untagged"), true);
            }
            State.DepotGuid.Invalidate();
        }
    }
    // Probe only after the complete station set is installed. A resource id can be an alias or use
    // the client's language while a dedicated server runs in English, so resolution can fall back
    // to the sole cargo class present at the matching LOAD producer.
    for (const FDTTDispatchStation& Station : DispatchStations)
    {
        if (const TSubclassOf<UFGItemDescriptor> ItemClass = ResolveItemClassForResource(Station.ResourceId))
        {
            const int32 Stock = ReadDockStockInStacks(Station.DockingStationIdentifier.Get(), ItemClass);
            FDTTRequestLevels Levels;
            const bool bLevels = GetRequestLevels(Station, ItemClass, Levels);
            UE_LOG(LogDynamicTimeTableSubsystem, VeryVerbose,
                TEXT("DISPATCH_STOCK_PROBE Name=\"%s\" Resource=\"%s\" Item=\"%s\" StockStacks=%d Capacity=%d Trigger=%d Target=%d"),
                *Station.DisplayName, *Station.ResourceId, *UFGItemDescriptor::GetItemName(ItemClass).ToString(),
                Stock, bLevels ? Levels.Capacity : INDEX_NONE, bLevels ? Levels.Trigger : INDEX_NONE, bLevels ? Levels.Target : INDEX_NONE);
            // A number the dock cannot hold - more stacks than its capacity, more than 100 % of a
            // tank - is clamped to "called while not full". Most likely a tag written under the
            // 1.1.0 meaning (N stacks kept free). Said once per rebuild, which only runs on a name change.
            if (GetEffectiveDispatchRole(Station) == EDTTDispatchRole::Requester && bLevels && Station.Threshold != INDEX_NONE &&
                Station.Threshold > (Levels.bFluid ? 100 : Levels.Capacity))
            {
                UE_LOG(LogDynamicTimeTableSubsystem, Warning,
                    TEXT("DISPATCH_TAG_LEVEL_CLAMPED Name=\"%s\" Resource=\"%s\" N=%d Max=%d Unit=%s Action=called-while-not-full"),
                    *Station.DisplayName, *Station.ResourceId, Station.Threshold, Levels.bFluid ? 100 : Levels.Capacity,
                    Levels.bFluid ? TEXT("percent") : TEXT("stacks"));
            }
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
    for (auto It = RequesterOpenSinceWorldSeconds.CreateIterator(); It; ++It)
    {
        const bool bStillRequester = DispatchStations.ContainsByPredicate([&](const FDTTDispatchStation& S)
            { return GetEffectiveDispatchRole(S) == EDTTDispatchRole::Requester && S.DockingPathNodeGuid == It.Key(); });
        if (!bStillRequester) { OverdueRequestsLogged.Remove(It.Key()); It.RemoveCurrent(); }
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
int32 UDynamicTimeTableSubsystem::GetDockAssignedCount(AFGDockingStationIdentifier* Id,const AFGWheeledVehicleIdentifier* Exclude) const{
    if(!IsValid(Id))return 0;
    int32 C=0;
    for(const auto&P:TruckBookings)if(P.Key.IsValid()&&P.Key.Get()!=Exclude&&P.Value.Get()==Id)++C;
    // A dispatch truck books only its producer (ChooseProducerFromGroup), never a fuel stop, so a
    // borrowed, emergency or dedicated (F) it currently holds has to be counted here too, or
    // SelectBestStation would be blind to its claim on a fuel group. An own (F) counts only on laps
    // that actually include it: the anchor outlives laps that leave it out.
    const FGuid PathNodeGuid=Id->GetPathNodeGUID();
    if(PathNodeGuid.IsValid())
    {
        for(const auto&P:DispatchTruckAnchors)
        {
            const AFGWheeledVehicleIdentifier* Vehicle=P.Key.Get();
            if(!IsValid(Vehicle)||Vehicle==Exclude)continue;
            if(P.Value.BorrowedFuelGuid==PathNodeGuid||P.Value.EmergencyFuelGuid==PathNodeGuid||
               (P.Value.FuelGuid==PathNodeGuid&&Vehicle->GetVehicleRoute().Contains(PathNodeGuid)))++C;
        }
        // A pool lap's (F) is the same kind of claim; it lasts while the stop is still in the route
        // (CutLapIfCargoEmpty may have dropped it) and the lap is running.
        for(const auto&P:PoolTrucks)
        {
            const AFGWheeledVehicleIdentifier* Vehicle=P.Key.Get();
            if(!IsValid(Vehicle)||Vehicle==Exclude||P.Value.Phase!=EDTTTruckPhase::Assigned)continue;
            if((P.Value.FuelGuid==PathNodeGuid||P.Value.EmergencyFuelGuid==PathNodeGuid)&&
               Vehicle->GetVehicleRoute().Contains(PathNodeGuid))++C;
        }
    }
    return C;
}
int32 UDynamicTimeTableSubsystem::GetEntryAssignedCount(const FDTTStationGroup& G,const FDTTStationEntry& E,const AFGWheeledVehicleIdentifier* Exclude) const{return G.Kind==EDTTGroupKind::Truck?GetDockAssignedCount(E.DockingStationIdentifier.Get(),Exclude):GetAssignedCount(E.StationIdentifier.Get());}

FDTTStationEntry* UDynamicTimeTableSubsystem::SelectBestStation(FDTTStationGroup& G,const AFGWheeledVehicleIdentifier* Exclude)
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
        const int32 C=GetEntryAssignedCount(G,E,Exclude);
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
            DestinationName = StripAllTags(Destination->GetStationName().ToString());
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
    const FGuid Emergency = Existing ? Existing->EmergencyFuelGuid : FGuid();
    FGuid FuelGuid;
    int32 FuelInsertIndex = INDEX_NONE;
    int32 FuelTagsInRoute = 0;
    for (int32 Index = 1; Index < Route.Num(); ++Index)
    {
        const FGuid& Candidate = Route[Index];
        if (!Candidate.IsValid() || Candidate == Borrowed || Candidate == Emergency ||
            !DispatchFuelStopGuids.Contains(Candidate)) continue;
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

    const TArray<FGuid> Before = Vehicle->GetVehicleRoute();
    InternalRouteChanges.Add(Vehicle);
    Vehicle->SetVehicleRoute(NewRoute);
    InternalRouteChanges.Remove(Vehicle);
    const bool bWritten = Vehicle->GetVehicleRoute() == NewRoute;
    // Keep the poll's copy current at once: the game may re-save the route before the next poll,
    // and that re-save must compare equal, not read as a player edit. The pre-write route is kept
    // too, to recognise the game writing it back after a docking (HandleVehicleRouteChanged).
    if (bWritten)
    {
        if (FDTTTruckPollState* Seen = TruckPollStates.Find(Vehicle))
        {
            Seen->LastRoute = NewRoute;
            Seen->LastWrittenRoute = NewRoute;
            Seen->RouteBeforeLastWrite = Before;
        }
    }
    return bWritten;
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
        // A pool lap starts and ends at the same depot: the closing leg is not driven, and the
        // path search has no path from a node to itself, which used to fail the whole estimate.
        if (From == To) continue;
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

    const float RequiredToSkip = Settings.FuelSkipMarginLaps * PerLapItems;
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
    const float RequiredWithMargin = Settings.FuelCriticalMarginLaps * PerLapItems;
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

FGuid UDynamicTimeTableSubsystem::ChooseFuelStopFromGroup(AFGWheeledVehicleIdentifier* Vehicle, const FGuid& Seed)
{
    if (!Seed.IsValid()) return Seed;
    FDTTStationGroup* Group = FindGroupByDock(FindDockByPathNode(Seed));
    if (!Group) return Seed;
    FDTTStationEntry* Best = SelectBestStation(*Group, Vehicle);
    if (!Best || !Best->DockingPathNodeGuid.IsValid()) return Seed;
    if (Best->DockingPathNodeGuid != Seed)
    {
        UE_LOG(LogDynamicTimeTableSubsystem, Display,
            TEXT("FUEL_GROUP_PICK Truck='%s' Group='%s' Seed='%s' Chosen='%s' Load=%d/%d"),
            *DTSVehicleName(Vehicle), *Group->GroupName, *DockDisplayName(Seed), *Best->DisplayName,
            GetEntryAssignedCount(*Group, *Best, Vehicle), Best->TargetSlots);
    }
    return Best->DockingPathNodeGuid;
}

bool UDynamicTimeTableSubsystem::GetRequestLevels(
    const FDTTDispatchStation& Station, TSubclassOf<UFGItemDescriptor> ItemClass, FDTTRequestLevels& Out) const
{
    // Limit plus buffer: the dock is called once it drops below the limit, and the delivery brings
    // it back up to the limit plus a buffer, which covers what is consumed while the truck is on
    // its way. With the shipped limits (40 of 48, 56 of 64) and buffer (8) a dock without a number
    // fills to full and is not called for a single missing stack.
    Out = FDTTRequestLevels();
    if (!ItemClass || !ReadDockCapacityInStacks(Station.DockingStationIdentifier.Get(), ItemClass, Out.Capacity)) return false;
    Out.bFluid = DTSIsFluidForm(UFGItemDescriptor::GetForm(ItemClass));
    if (Station.Threshold == INDEX_NONE)
    {
        Out.Trigger = Out.bFluid ? Settings.FluidRequestStacks : Settings.SolidRequestStacks;
    }
    else if (Out.bFluid)
    {
        // A fluid tag's N is a percent of the tank: (Oil-30) is called below 30 %.
        Out.Trigger = FMath::CeilToInt(double(Out.Capacity) * FMath::Clamp(Station.Threshold, 0, 100) / 100.0);
    }
    else
    {
        Out.Trigger = Station.Threshold;
    }
    Out.Trigger = FMath::Clamp(Out.Trigger, 0, Out.Capacity);
    Out.Target = FMath::Clamp(Out.Trigger + Settings.RequestBufferStacks, Out.Trigger, Out.Capacity);
    return true;
}

bool UDynamicTimeTableSubsystem::IsFluidTruck(AFGWheeledVehicleIdentifier* Vehicle) const
{
    AFGWheeledVehicle* WheeledVehicle = IsValid(Vehicle) ? Vehicle->GetOwnerVehicle() : nullptr;
    return IsValid(WheeledVehicle) && DTSIsFluidForm(IFGDockableInterface::Execute_GetDockInventoryResourceForm(WheeledVehicle));
}

bool UDynamicTimeTableSubsystem::EstimateTruckCargoStacks(
    AFGWheeledVehicleIdentifier* Vehicle, TSubclassOf<UFGItemDescriptor> ItemClass,
    int32& OutCapacityStacks, int32& OutOnBoardStacks) const
{
    OutCapacityStacks = 0;
    OutOnBoardStacks = 0;
    if (!IsValid(Vehicle) || !ItemClass) return false;
    AFGWheeledVehicle* WheeledVehicle = Vehicle->GetOwnerVehicle();
    UFGInventoryComponent* Storage = IsValid(WheeledVehicle) ? WheeledVehicle->GetStorageInventory() : nullptr;
    if (!IsValid(Storage) || Storage->GetSizeLinear() < 1) return false;

    // Slot size rather than stack size: a fluid truck has a single slot with an arbitrary size of
    // 3 200 000 L, which is 64 fluid stacks of 50 000 L - the unit ReadDockStockInStacks counts in.
    const int64 StackSize = FMath::Max(1, UFGItemDescriptor::GetStackSize(ItemClass));
    const int64 SlotSize = FMath::Max<int64>(1, Storage->GetSlotSize(0, ItemClass));
    OutCapacityStacks = static_cast<int32>(FMath::Max<int64>(1, (Storage->GetSizeLinear() * SlotSize) / StackSize));
    OutOnBoardStacks = static_cast<int32>(Storage->GetNumItems(ItemClass) / StackSize);
    return true;
}

void UDynamicTimeTableSubsystem::CollectOpenRequesters(
    const FString& ResourceId, TSubclassOf<UFGItemDescriptor> ItemClass,
    AFGWheeledVehicleIdentifier* ForVehicle, TArray<FDTTOpenRequester>& Out) const
{
    Out.Reset();
    if (!ItemClass) return;
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
            if (Claim->IsValid() && Claim->Get() != ForVehicle) continue;
        }

        const int32 StockStacks = ReadDockStockInStacks(Station.DockingStationIdentifier.Get(), ItemClass);
        FDTTRequestLevels Levels;
        const bool bLevels = GetRequestLevels(Station, ItemClass, Levels);
        UE_LOG(LogDynamicTimeTableSubsystem, VeryVerbose,
            TEXT("DISPATCH_CANDIDATE Truck='%s' Resource='%s' Station='%s' StockStacks=%d Trigger=%d Target=%d Open=%s"),
            *DTSVehicleName(ForVehicle), *ResourceId, *Station.DisplayName, StockStacks,
            bLevels ? Levels.Trigger : INDEX_NONE, bLevels ? Levels.Target : INDEX_NONE,
            bLevels && StockStacks < Levels.Trigger ? TEXT("true") : TEXT("false"));
        // Unreadable inventory: neither stock nor capacity is known; a trigger of 0 never opens.
        if (!bLevels) continue;
        if (StockStacks < Levels.Trigger)
        {
            FDTTOpenRequester Candidate;
            Candidate.Station = &Station;
            Candidate.StockStacks = StockStacks;
            Candidate.TriggerStacks = Levels.Trigger;
            Candidate.TargetStacks = Levels.Target;
            Candidate.FillRatio = static_cast<double>(StockStacks) / static_cast<double>(Levels.Trigger);
            Out.Add(Candidate);
        }
    }

    // Lowest stock/threshold ratio is the most starved requester. Name and GUID make ties stable
    // across subsystem rebuilds and server restarts.
    Out.Sort([](const FDTTOpenRequester& A, const FDTTOpenRequester& B)
    {
        if (!FMath::IsNearlyEqual(A.FillRatio, B.FillRatio)) return A.FillRatio < B.FillRatio;
        if (A.Station->DisplayName != B.Station->DisplayName)
            return A.Station->DisplayName < B.Station->DisplayName;
        return A.Station->DockingPathNodeGuid.ToString() < B.Station->DockingPathNodeGuid.ToString();
    });
}

TArray<FGuid> UDynamicTimeTableSubsystem::SelectRequestersForResource(
    const FString& ResourceId, AFGWheeledVehicleIdentifier* Vehicle,
    const FGuid& ProducerGuid, TMap<FGuid, int32>* OutPlannedItems, int32* OutNeededStacks, bool bIgnoreCargo)
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

    TArray<FDTTOpenRequester> Candidates;
    CollectOpenRequesters(ResourceId, ItemClass, Vehicle, Candidates);
    if (Candidates.Num() == 0)
    {
        // No lap planned: the previous lap's plan must not outlive it, or the kept-last loop
        // would treat the old last requester as still planned and drain the truck into it.
        if (!OutPlannedItems)
            if (FDTTDispatchTruckAnchors* Anchors = DispatchTruckAnchors.Find(Vehicle)) Anchors->PlannedItems.Reset();
        return Result;
    }

    // What this lap can deliver, in stacks: the truck's capacity, but no more than what is already
    // on board plus what the producer holds right now. Unknown (no vehicle actor, no inventory)
    // degrades to a single requester, which is what every lap got before 1.0.9.
    int32 CapacityStacks = 0, OnBoardStacks = 0, AvailableStacks = 1;
    if (EstimateTruckCargoStacks(Vehicle, ItemClass, CapacityStacks, OnBoardStacks))
    {
        // Cargo about to go into an UNLOAD depot is not available to this lap.
        if (bIgnoreCargo) OnBoardStacks = 0;
        // A pool truck without a producer delivers from what it carries; a legacy lap always
        // loads at Route[0].
        const TArray<FGuid>& Route = Vehicle->GetVehicleRoute();
        const bool bPoolTruck = PoolTrucks.Contains(Vehicle);
        const FGuid Producer = ProducerGuid.IsValid() ? ProducerGuid : (!bPoolTruck && Route.Num() > 0 ? Route[0] : FGuid());
        const int32 ProducerStacks = Producer.IsValid() ? ReadDockStockInStacks(FindDockByPathNode(Producer), ItemClass) : 0;
        AvailableStacks = FMath::Max(1, FMath::Min(CapacityStacks, OnBoardStacks + ProducerStacks));
    }

    const UWorld* World = GetWorld();
    const double ClaimedAt = IsValid(World) ? World->GetTimeSeconds() : 0.0;
    // Most starved first, and as many as the cargo covers: each requester's deficit is what it
    // takes to bring it back up to its threshold. The first is always taken; a later one only
    // while the ones before it leave something on board. A truck that empties early anyway cuts
    // the rest of the lap (CutLapIfCargoEmpty), so overshooting here costs one detour at most.
    int32 CoveredDeficit = 0;
    FDTTDispatchTruckAnchors* Anchors = OutPlannedItems ? nullptr : DispatchTruckAnchors.Find(Vehicle);
    TMap<FGuid, int32>* Planned = OutPlannedItems ? OutPlannedItems : (Anchors ? &Anchors->PlannedItems : nullptr);
    if (Planned) Planned->Reset();
    const int32 StackSize = FMath::Max(1, UFGItemDescriptor::GetStackSize(ItemClass));
    // A fluid lap under "fluid full load" serves one requester: the tank is filled completely and
    // drained there, so that stop must be the last of the lap.
    const int32 MaxRequesters = Settings.bFluidFullLoad && IsFluidTruck(Vehicle) ? 1 : Settings.MaxRequestersPerLap;
    for (int32 Priority = 0; Priority < Candidates.Num() && Result.Num() < MaxRequesters; ++Priority)
    {
        if (Priority > 0 && CoveredDeficit >= AvailableStacks) break;
        const FDTTOpenRequester& Candidate = Candidates[Priority];
        const FDTTDispatchStation* Selected = Candidate.Station;
        RequesterClaims.Add(Selected->DockingPathNodeGuid, Vehicle);
        RequesterClaimedAtWorldSeconds.Add(Selected->DockingPathNodeGuid, ClaimedAt);
        Result.Add(Selected->DockingPathNodeGuid);
        CoveredDeficit += FMath::Max(1, Candidate.TargetStacks - Candidate.StockStacks);
        if (Planned) Planned->Add(Selected->DockingPathNodeGuid, FMath::Max(0, Candidate.TargetStacks - Candidate.StockStacks) * StackSize);
        UE_LOG(LogDynamicTimeTableSubsystem, Verbose,
            TEXT("DISPATCH_CLAIMED Truck='%s' Resource='%s' Requester='%s' Guid=%s Priority=%d StockStacks=%d Trigger=%d Target=%d FillRatio=%.6f"),
            *DTSVehicleName(Vehicle), *ResourceId, *Selected->DisplayName,
            *Selected->DockingPathNodeGuid.ToString(), Priority, Candidate.StockStacks,
            Candidate.TriggerStacks, Candidate.TargetStacks, Candidate.FillRatio);
    }
    UE_LOG(LogDynamicTimeTableSubsystem, Verbose,
        TEXT("DISPATCH_LAP_PLANNED Truck='%s' Resource='%s' Open=%d Requesters=%d Deficit=%d Cargo=%d OnBoard=%d Capacity=%d"),
        *DTSVehicleName(Vehicle), *ResourceId, Candidates.Num(), Result.Num(), CoveredDeficit,
        AvailableStacks, OnBoardStacks, CapacityStacks);
    if (OutNeededStacks) *OutNeededStacks = CoveredDeficit;
    return Result;
}

FGuid UDynamicTimeTableSubsystem::ChooseProducerFromGroup(
    AFGWheeledVehicleIdentifier* Vehicle, const FString& ResourceId, const FGuid& CurrentProducer)
{
    FDTTStationGroup* Group = FindGroupByDock(FindDockByPathNode(CurrentProducer));
    if (!Group) return CurrentProducer;

    // The truck's own booking (a producer docking that was never observed) must not weigh on the
    // dock it is about to choose from.
    ReleaseTruckBooking(Vehicle, TEXT("producer boundary"));

    const TSubclassOf<UFGItemDescriptor> ItemClass = ResolveItemClassForResource(ResourceId);
    // Same tolerance as SelectBestStation: the ratio is computed, never stored, and 1/3 has to
    // match 1/3.
    constexpr double RatioTolerance = 1e-6;
    const FDTTStationEntry* Best = nullptr;
    double BestRatio = TNumericLimits<double>::Max();
    int32 BestStock = INDEX_NONE;
    int32 BestLoad = 0;
    int32 Candidates = 0;
    for (const FDTTStationEntry& Entry : Group->Stations)
    {
        AFGDockingStationIdentifier* Dock = Entry.DockingStationIdentifier.Get();
        if (!IsValid(Dock) || Entry.TargetSlots <= 0) continue;
        const FGuid Guid = Dock->GetPathNodeGUID();
        // Only a LOAD producer of this resource can stand in for Route[0]: a member of the group
        // that is in UNLOAD, or tagged with another resource, is not a place to load at.
        const bool bProducer = DispatchStations.ContainsByPredicate([&](const FDTTDispatchStation& Station)
        {
            return Station.DockingPathNodeGuid == Guid &&
                Station.ResourceId.Equals(ResourceId, ESearchCase::IgnoreCase) &&
                GetEffectiveDispatchRole(Station) == EDTTDispatchRole::Producer &&
                DoesDispatchStationMatchMode(Station);
        });
        if (!bProducer) continue;
        ++Candidates;
        const int32 Load = GetDockAssignedCount(Dock);
        const double Ratio = double(Load) / double(Entry.TargetSlots);
        const int32 Stock = ItemClass ? ReadDockStockInStacks(Dock, ItemClass) : 0;
        UE_LOG(LogDynamicTimeTableSubsystem, VeryVerbose,
            TEXT("DISPATCH_PRODUCER_CANDIDATE Truck='%s' Producer='%s' Load=%d/%d Ratio=%.3f Stock=%d"),
            *DTSVehicleName(Vehicle), *Entry.DisplayName, Load, Entry.TargetSlots, Ratio, Stock);
        // Load first, as in every group; a tie goes to the fuller station rather than a rotation
        // cursor, and to the current producer when the stock ties too (no rewrite for nothing).
        bool bBetter = Ratio < BestRatio - RatioTolerance;
        if (!bBetter && Ratio <= BestRatio + RatioTolerance)
        {
            bBetter = Stock > BestStock || (Stock == BestStock && Guid == CurrentProducer);
        }
        if (bBetter)
        {
            Best = &Entry;
            BestRatio = Ratio;
            BestStock = Stock;
            BestLoad = Load;
        }
    }
    if (!Best) return CurrentProducer;

    AFGDockingStationIdentifier* ChosenDock = Best->DockingStationIdentifier.Get();
    const FGuid Chosen = ChosenDock->GetPathNodeGUID();
    // Booked like any group target; released when the truck docks there (PollTruckStates).
    TruckBookings.Add(Vehicle, ChosenDock);
    OnGroupsChanged.Broadcast(); OnGroupsChangedNative.Broadcast();
    UE_LOG(LogDynamicTimeTableSubsystem, Verbose,
        TEXT("DISPATCH_PRODUCER_SELECTED Truck='%s' Resource='%s' Group='%s' Producer='%s' Changed=%s Load=%d/%d Stock=%d Candidates=%d"),
        *DTSVehicleName(Vehicle), *ResourceId, *Group->GroupName, *Best->DisplayName,
        Chosen != CurrentProducer ? TEXT("true") : TEXT("false"), BestLoad + 1, Best->TargetSlots, BestStock, Candidates);
    return Chosen;
}

int32 UDynamicTimeTableSubsystem::ChooseFuelInsertIndex(
    AFGWheeledVehicleIdentifier* Vehicle, const TArray<FGuid>& BaseLoop,
    const FGuid& FuelGuid, int32 FallbackIndex) const
{
    const int32 Num = BaseLoop.Num();
    const int32 Fallback = FMath::Clamp(FallbackIndex, 1, FMath::Max(1, Num));
    if (!IsValid(Vehicle) || Num < 2 || !FuelGuid.IsValid()) return Fallback;

    AFGWheeledVehicle* WheeledVehicle = Vehicle->GetOwnerVehicle();
    if (!IsValid(WheeledVehicle)) return Fallback;
    const UFGVehiclePathPreset* Preset = WheeledVehicle->GetVehiclePathPreset();
    AFGVehicleSubsystem* VehicleSubsystem = AFGVehicleSubsystem::Get(WheeledVehicle->GetWorld());
    if (!Preset || !IsValid(VehicleSubsystem)) return Fallback;

    const TSubclassOf<UFGItemDescriptor> FuelClass = Vehicle->GetFuelTypeDescriptor();
    UFGInventoryComponent* FuelInventory = WheeledVehicle->GetFuelInventory();
    const float EnergyPerItem = FuelClass ? UFGItemDescriptor::GetEnergyValue(FuelClass) : 0.0f;
    if (!IsValid(FuelInventory) || !FuelClass || !FMath::IsFinite(EnergyPerItem) || EnergyPerItem <= 0.0f) return Fallback;
    const float ReachableEnergy = FuelInventory->GetNumItems(FuelClass) * EnergyPerItem * FuelReachFraction;

    auto Cost = [&](const FGuid& From, const FGuid& To) -> float
    {
        return From == To ? 0.0f : EstimatePathFuelConsumptionMW(VehicleSubsystem, Preset, WheeledVehicle->GetClass(), From, To);
    };

    // Leg i of the base loop runs BaseLoop[i] -> BaseLoop[(i + 1) % Num].
    TArray<float> Legs;
    Legs.SetNumZeroed(Num);
    for (int32 LegIndex = 0; LegIndex < Num; ++LegIndex)
    {
        Legs[LegIndex] = Cost(BaseLoop[LegIndex], BaseLoop[(LegIndex + 1) % Num]);
        if (Legs[LegIndex] < 0.0f) return Fallback;
    }

    // Inserting before BaseLoop[k] (k == Num: before the return to the producer) replaces leg k-1
    // with two legs through the fuel stop; the detour is what that adds. The tank must reach the
    // stop first: at the boundary the truck is still driving the last leg back to the producer, so
    // that leg is spent before anything else.
    float SpentBeforeLeg = Legs[Num - 1];
    int32 Best = INDEX_NONE;
    float BestExtra = TNumericLimits<float>::Max();
    for (int32 Position = 1; Position <= Num; ++Position)
    {
        const FGuid& Prev = BaseLoop[Position - 1];
        const FGuid& Next = BaseLoop[Position % Num];
        const float ToFuel = Cost(Prev, FuelGuid);
        const float FromFuel = Cost(FuelGuid, Next);
        if (ToFuel < 0.0f || FromFuel < 0.0f) return Fallback;
        const float Extra = ToFuel + FromFuel - Legs[Position - 1];
        const bool bReachable = SpentBeforeLeg + ToFuel <= ReachableEnergy;
        UE_LOG(LogDynamicTimeTableSubsystem, VeryVerbose,
            TEXT("DISPATCH_FUEL_POSITION Truck='%s' Index=%d Extra=%.3f EnergyToReach=%.3f Reachable=%s"),
            *DTSVehicleName(Vehicle), Position, Extra, SpentBeforeLeg + ToFuel, bReachable ? TEXT("true") : TEXT("false"));
        if (bReachable && Extra < BestExtra)
        {
            BestExtra = Extra;
            Best = Position;
        }
        SpentBeforeLeg += Legs[Position - 1];
    }

    // Nothing reachable at 80% of the tank: go for the pump first, right after loading - the
    // closest the stop can be from where the truck is now.
    const int32 Chosen = Best == INDEX_NONE ? 1 : Best;
    UE_LOG(LogDynamicTimeTableSubsystem, Verbose,
        TEXT("DISPATCH_FUEL_PLACED Truck='%s' Fuel=%s Index=%d Extra=%.3f Reachable=%s ReachableEnergy=%.3f LoopStops=%d"),
        *DTSVehicleName(Vehicle), *FuelGuid.ToString(), Chosen,
        Best == INDEX_NONE ? -1.0f : BestExtra, Best == INDEX_NONE ? TEXT("false") : TEXT("true"),
        ReachableEnergy, Num);
    return Chosen;
}

bool UDynamicTimeTableSubsystem::CutLapIfCargoEmpty(
    AFGWheeledVehicleIdentifier* Vehicle, const FString& ResourceId,
    const TArray<FGuid>& Route, int32 Index, const TCHAR* Trigger, const FGuid& KeepTail)
{
    // Index 0 is the boundary and index 1 the first stop after loading; only from the second stop
    // on can a delivery already have happened. Reached from the undock transition in
    // PollTruckStates: the index had already moved on when the truck docked. The target itself has to be a requester of this
    // resource: heading for the producer or an (F) there is nothing to cut short.
    if (!IsValid(Vehicle) || Index < 2 || !Route.IsValidIndex(Index)) return false;
    auto IsRequesterOfResource = [&](const FGuid& Guid) -> bool
    {
        return Guid.IsValid() && DispatchStations.ContainsByPredicate([&](const FDTTDispatchStation& Station)
        {
            return Station.DockingPathNodeGuid == Guid &&
                Station.ResourceId.Equals(ResourceId, ESearchCase::IgnoreCase) &&
                GetEffectiveDispatchRole(Station) == EDTTDispatchRole::Requester;
        });
    };
    if (!IsRequesterOfResource(Route[Index])) return false;
    bool bDeliveredBefore = false;
    for (int32 RouteIndex = 1; RouteIndex < Index && !bDeliveredBefore; ++RouteIndex)
    {
        bDeliveredBefore = IsRequesterOfResource(Route[RouteIndex]);
    }
    if (!bDeliveredBefore) return false;

    const TSubclassOf<UFGItemDescriptor> ItemClass = ResolveItemClassForResource(ResourceId);
    AFGWheeledVehicle* WheeledVehicle = Vehicle->GetOwnerVehicle();
    UFGInventoryComponent* Storage = IsValid(WheeledVehicle) ? WheeledVehicle->GetStorageInventory() : nullptr;
    if (!ItemClass || !IsValid(Storage)) return false;
    const int32 OnBoard = Storage->GetNumItems(ItemClass);
    // Strictly empty. Anything at all on board is still worth the next stop.
    if (OnBoard > 0) return false;

    // Everything before the current target stays; the rest of the lap goes. The truck's target
    // index is now past the end of its route - what the autopilot does with that is the game's
    // business (the vanilla UI removes waypoints from a driving truck the same way), and the next
    // poll sees whichever index it settles on. A pool lap keeps its tail: the depot it returns to.
    TArray<FGuid> NewRoute(Route.GetData(), Index);
    if (KeepTail.IsValid() && !NewRoute.Contains(KeepTail)) NewRoute.Add(KeepTail);
    if (NewRoute.Num() < 2) return false;
    if (!ReplaceRoute(Vehicle, NewRoute))
    {
        UE_LOG(LogDynamicTimeTableSubsystem, Warning,
            TEXT("DISPATCH_LAP_CUT_FAILED Truck='%s' Resource='%s' Trigger=%s"),
            *DTSVehicleName(Vehicle), *ResourceId, Trigger);
        return false;
    }

    // Claims only after the write took: a dropped stop is free for the next truck.
    for (int32 RouteIndex = Index; RouteIndex < Route.Num(); ++RouteIndex)
    {
        const FGuid& Dropped = Route[RouteIndex];
        if (Dropped == KeepTail) continue;
        const TWeakObjectPtr<AFGWheeledVehicleIdentifier>* Owner = RequesterClaims.Find(Dropped);
        if (!Owner || Owner->Get() != Vehicle) continue;
        RequesterClaims.Remove(Dropped);
        RequesterClaimedAtWorldSeconds.Remove(Dropped);
        UE_LOG(LogDynamicTimeTableSubsystem, Verbose,
            TEXT("DISPATCH_CLAIM_RELEASED Truck='%s' RequesterGuid=%s Reason=cargo-empty"),
            *DTSVehicleName(Vehicle), *Dropped.ToString());
    }
    UE_LOG(LogDynamicTimeTableSubsystem, Verbose,
        TEXT("DISPATCH_LAP_CUT Truck='%s' Resource='%s' Kept=%d Dropped=%d Trigger=%s"),
        *DTSVehicleName(Vehicle), *ResourceId, NewRoute.Num(), Route.Num() - Index, Trigger);
    return true;
}

bool UDynamicTimeTableSubsystem::TankCoversLeg(
    AFGWheeledVehicleIdentifier* Vehicle, const FGuid& From, const FGuid& To,
    float ExtraCost, float& OutNeed, float& OutEnergy) const
{
    OutNeed = -1.0f;
    OutEnergy = -1.0f;
    AFGWheeledVehicle* WheeledVehicle = IsValid(Vehicle) ? Vehicle->GetOwnerVehicle() : nullptr;
    if (!IsValid(WheeledVehicle) || !From.IsValid() || !To.IsValid() || From == To) return true;
    const UFGVehiclePathPreset* Preset = WheeledVehicle->GetVehiclePathPreset();
    AFGVehicleSubsystem* VehicleSubsystem = AFGVehicleSubsystem::Get(WheeledVehicle->GetWorld());
    const TSubclassOf<UFGItemDescriptor> FuelClass = Vehicle->GetFuelTypeDescriptor();
    UFGInventoryComponent* FuelInventory = WheeledVehicle->GetFuelInventory();
    const float EnergyPerItem = FuelClass ? UFGItemDescriptor::GetEnergyValue(FuelClass) : 0.0f;
    // No estimate, no verdict: an unknown leg is left alone, as the lap planner leaves it.
    if (!Preset || !IsValid(VehicleSubsystem) || !IsValid(FuelInventory) || !FuelClass ||
        !FMath::IsFinite(EnergyPerItem) || EnergyPerItem <= 0.0f) return true;
    const float LegCost = EstimatePathFuelConsumptionMW(VehicleSubsystem, Preset, WheeledVehicle->GetClass(), From, To);
    if (LegCost < 0.0f) return true;
    OutNeed = LegCost + FMath::Max(0.0f, ExtraCost);
    OutEnergy = FuelInventory->GetNumItems(FuelClass) * EnergyPerItem;
    return OutEnergy >= FuelLegMarginFactor * OutNeed;
}

bool UDynamicTimeTableSubsystem::EnsureFuelForNextLeg(
    AFGWheeledVehicleIdentifier* Vehicle, bool bClosedLoop, FGuid& OutEmergencyFuel, const TCHAR* Trigger,
    const FGuid& StandingAt)
{
    OutEmergencyFuel.Invalidate();
    if (!IsValid(Vehicle)) return false;
    const TArray<FGuid> Route = Vehicle->GetVehicleRoute();
    const int32 Num = Route.Num();
    const int32 Index = Vehicle->GetCurrentTargetWaypointIndex();
    if (Num < 2 || Index < 0) return false;
    // Index zero of a dispatch loop is the boundary, planned as a whole; a pool lap released from
    // its depot with the target at zero is guarded from the depot the caller names.
    if (Index == 0 && (bClosedLoop || !StandingAt.IsValid())) return false;

    // The index moves on at arrival (PollTruckStates), so the truck stands at the stop before its
    // target. Past the end - CutLapIfCargoEmpty just shortened the route - the autopilot wraps to
    // Route[0]; a dispatch loop then drives back to its producer, a pool lap never gets here.
    const bool bWrapped = Index >= Num;
    if (bWrapped && !bClosedLoop) return false;
    const int32 ToIndex = bWrapped ? 0 : Index;
    const FGuid From = StandingAt.IsValid() ? StandingAt : Route[bWrapped ? Num - 1 : Index - 1];
    const FGuid To = Route[ToIndex];
    if (!From.IsValid() || !To.IsValid() || From == To) return false;
    // Standing at a pump, or heading for one: the tank is about to be filled.
    if (DispatchFuelStopGuids.Contains(From) || DispatchFuelStopGuids.Contains(To)) return false;

    // The last stop of a dispatch loop is followed by the leg back to Route[0], and nothing can be
    // inserted in front of index zero (it is the truck's producer, its dispatch identity): that
    // closing leg is protected one stop early, together with the leg to the last stop.
    float ClosingCost = 0.0f;
    // And after the leg the truck must still be able to reach a pump: the detour from the target
    // to the nearest (F) is demanded on top of the doubled leg, at the planner's reach fraction.
    // Two short legs in a row each passed a bare 2x check and left a truck with 2.5 items.
    float PumpAfterCost = 0.0f;
    {
        AFGWheeledVehicle* WheeledVehicle = Vehicle->GetOwnerVehicle();
        const UFGVehiclePathPreset* Preset = IsValid(WheeledVehicle) ? WheeledVehicle->GetVehiclePathPreset() : nullptr;
        AFGVehicleSubsystem* VehicleSubsystem = IsValid(WheeledVehicle) ? AFGVehicleSubsystem::Get(WheeledVehicle->GetWorld()) : nullptr;
        if (Preset && IsValid(VehicleSubsystem))
        {
            const FGuid LastLegEnd = bClosedLoop && !bWrapped && ToIndex == Num - 1 && To != Route[0] ? Route[0] : To;
            if (LastLegEnd != To)
            {
                ClosingCost = FMath::Max(0.0f, EstimatePathFuelConsumptionMW(VehicleSubsystem, Preset, WheeledVehicle->GetClass(), To, LastLegEnd));
            }
            const FGuid PumpAfter = FindNearestFuelStop(WheeledVehicle, LastLegEnd);
            if (PumpAfter.IsValid())
            {
                PumpAfterCost = FMath::Max(0.0f, EstimatePathFuelConsumptionMW(VehicleSubsystem, Preset, WheeledVehicle->GetClass(), LastLegEnd, PumpAfter)) / FuelReachFraction;
            }
        }
    }

    float Need = 0.0f, Energy = 0.0f;
    // TankCoversLeg supplies the cost and the energy; the pump detour is added once, undoubled.
    TankCoversLeg(Vehicle, From, To, ClosingCost, Need, Energy);
    if (Need < 0.0f || Energy >= FuelLegMarginFactor * Need + PumpAfterCost) return false;

    const FGuid Fuel = ChooseFuelStopFromGroup(Vehicle, FindNearestFuelStop(Vehicle->GetOwnerVehicle(), From));
    if (!Fuel.IsValid())
    {
        UE_LOG(LogDynamicTimeTableSubsystem, Warning,
            TEXT("FUEL_LEG_GUARD_NO_STOP Truck='%s' From='%s' To='%s' LegCost=%.3f Energy=%.3f Trigger=%s"),
            *DTSVehicleName(Vehicle), *DockDisplayName(From), *DockDisplayName(To), Need, Energy, Trigger);
        return false;
    }
    // Not a reason to skip the detour - a pump the tank may not reach still beats a leg it
    // certainly cannot - but worth a line in the log.
    float ToFuelNeed = 0.0f, ToFuelEnergy = 0.0f;
    const bool bReachable = TankCoversLeg(Vehicle, From, Fuel, 0.0f, ToFuelNeed, ToFuelEnergy) ||
        (ToFuelNeed >= 0.0f && ToFuelNeed <= Energy * FuelReachFraction);
    if (!bReachable)
    {
        UE_LOG(LogDynamicTimeTableSubsystem, Warning,
            TEXT("FUEL_LEG_GUARD_FAR Truck='%s' Fuel='%s' ToFuelCost=%.3f Energy=%.3f Trigger=%s"),
            *DTSVehicleName(Vehicle), *DockDisplayName(Fuel), ToFuelNeed, Energy, Trigger);
    }

    // In front of the target; after the last stop when the index has already wrapped, so the
    // truck drives From -> Fuel -> Route[0] instead of straight home.
    TArray<FGuid> NewRoute = Route;
    if (bWrapped) NewRoute.Add(Fuel);
    else NewRoute.Insert(Fuel, ToIndex);
    if (!ReplaceRoute(Vehicle, NewRoute))
    {
        UE_LOG(LogDynamicTimeTableSubsystem, Warning,
            TEXT("FUEL_LEG_GUARD_FAILED Truck='%s' Fuel='%s' Trigger=%s"),
            *DTSVehicleName(Vehicle), *DockDisplayName(Fuel), Trigger);
        return false;
    }
    OutEmergencyFuel = Fuel;
    UE_LOG(LogDynamicTimeTableSubsystem, Display,
        TEXT("FUEL_LEG_GUARD Truck='%s' From='%s' To='%s' LegCost=%.3f PumpAfter=%.3f Energy=%.3f Fuel='%s' At=%d Reachable=%s Trigger=%s"),
        *DTSVehicleName(Vehicle), *DockDisplayName(From), *DockDisplayName(To), Need, PumpAfterCost, Energy,
        *DockDisplayName(Fuel), bWrapped ? Num : ToIndex, bReachable ? TEXT("true") : TEXT("false"), Trigger);
    return true;
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
    if (Index > 0)
    {
        // The producer booking normally went at docking there; this closes the gap if that
        // transition was missed, so the slot is not held for a whole lap.
        ReleaseTruckBooking(Vehicle, TEXT("left producer"));
        // Two mid-lap decisions. The cut only ever shortens the route: an empty truck heading for
        // yet another requester turns back to the producer instead. The leg guard runs on the route
        // the cut left behind - the cut drops an (F) that stood after the current stop, which is
        // how a lap planned around a pump ended with the truck dry on the way back.
        CutLapIfCargoEmpty(Vehicle, ResourceId, Route, Index, Trigger);
        if (FDTTDispatchTruckAnchors* Anchors = DispatchTruckAnchors.Find(Vehicle))
        {
            FGuid Emergency;
            if (EnsureFuelForNextLeg(Vehicle, true, Emergency, Trigger)) Anchors->EmergencyFuelGuid = Emergency;
        }
        return;
    }
    if (Index != 0 || Route.Num() < 1) return;

    if (!UpdateRememberedAnchors(Vehicle, ResourceId, Route)) return;

    FDTTDispatchTruckAnchors* Anchors = DispatchTruckAnchors.Find(Vehicle);
    if (!Anchors || !Anchors->ProducerGuid.IsValid()) return;

    // With the producer in a [group], this lap loads at whichever LOAD producer of the group the
    // group rule picks. That is this mod's own change of Route[0], not a hand edit, so the anchors
    // follow it here and UpdateRememberedAnchors sees them agree at the next boundary.
    const FGuid ChosenProducer = ChooseProducerFromGroup(Vehicle, ResourceId, Anchors->ProducerGuid);
    if (ChosenProducer.IsValid() && ChosenProducer != Anchors->ProducerGuid) Anchors->ProducerGuid = ChosenProducer;

    // Index zero starts a new lap. A docking transition normally released the previous claim;
    // this also closes the gap if that transition was missed while the actor was not observable.
    ReleaseRequesterClaimsForVehicle(Vehicle, TEXT("producer boundary"));
    const TArray<FGuid> Requesters = SelectRequestersForResource(ResourceId, Vehicle);

    // The loop this lap delivers on. It never contains a fuel stop, so the fuel estimate is always
    // taken on it directly. With no demand the truck keeps Producer and the last consumer of the
    // previous lap - one stop, not the whole multi-stop lap replayed empty - minus whichever (F)
    // last lap added.
    TArray<FGuid> BaseLoop;
    BaseLoop.Add(Anchors->ProducerGuid);
    if (Requesters.Num() > 0)
    {
        BaseLoop.Append(Requesters);
    }
    else
    {
        for (int32 RouteIndex = Route.Num() - 1; RouteIndex >= 1; --RouteIndex)
        {
            const FGuid& Node = Route[RouteIndex];
            if (!Node.IsValid() || Node == Anchors->ProducerGuid ||
                Node == Anchors->FuelGuid || Node == Anchors->BorrowedFuelGuid ||
                Node == Anchors->EmergencyFuelGuid) continue;
            BaseLoop.Add(Node);
            break;
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
    const FGuid PreviousEmergency = Anchors->EmergencyFuelGuid;
    // Cleared before any selection so this truck's own stale borrow does not count against the
    // fuel group it is about to choose from (GetDockAssignedCount). The emergency stop of the lap
    // that just ended goes the same way: the leg guard decides afresh on every leg.
    Anchors->BorrowedFuelGuid.Invalidate();
    Anchors->EmergencyFuelGuid.Invalidate();
    bool bFuelIncluded = false;
    int32 FuelPlacedIndex = INDEX_NONE;

    if (bHasDedicatedFuel)
    {
        // A player's own (F). In the lap only when the tank needs it; the anchor keeps it for the
        // laps that leave it out. Where it goes is decided per lap: the position with the smallest
        // detour that the tank can still reach, since with several consumers the pump may well sit
        // closer to the second than to the first. The remembered position is the fallback when the
        // path network cannot be costed. If the same dock is this lap's requester it is visited
        // anyway - a delivery is never dropped just because the dock also carries (F).
        if (BaseLoop.Contains(Anchors->FuelGuid))
        {
            bFuelIncluded = true;
        }
        else if (!ShouldSkipFuelStop(Vehicle, BaseLoop))
        {
            // The own (F) names the group as much as the station: when it carries a [tag,N], the
            // lap goes to whichever member is least loaded right now (measured 14.09.2026: eight
            // trucks pinned to one member of a three-station group, 5/2 booked while its neighbours
            // idled). A sibling rides in BorrowedFuelGuid for this lap - counted like any claim,
            // never adopted as the truck's own - and FuelGuid stays the home anchor.
            const FGuid Chosen = ChooseFuelStopFromGroup(Vehicle, Anchors->FuelGuid);
            if (Chosen != Anchors->FuelGuid) Anchors->BorrowedFuelGuid = Chosen;
            FuelPlacedIndex = FMath::Clamp(
                ChooseFuelInsertIndex(Vehicle, BaseLoop, Chosen, Anchors->FuelInsertIndex), 1, NewRoute.Num());
            NewRoute.Insert(Chosen, FuelPlacedIndex);
            bFuelIncluded = true;
        }
    }
    else
    {
        // No (F) of this truck's own. Normally that is a deliberate choice (docs: assumed to refuel
        // at regular stations) - but if the tank is genuinely at risk this lap, borrow the nearest
        // reachable (F) instead of letting the truck strand. Decided afresh every lap: a borrowed
        // stop is dropped again as soon as the tank no longer needs it. The nearest (F) only names
        // the group; within it the least-loaded member is taken (ChooseFuelStopFromGroup).
        const bool bIsCriticallyLow = IsFuelCriticallyLow(Vehicle, BaseLoop);
        FGuid ChosenGuid;
        if (bIsCriticallyLow)
        {
            ChosenGuid = ChooseFuelStopFromGroup(Vehicle, FindNearestFuelStop(Vehicle->GetOwnerVehicle(), Anchors->ProducerGuid));
        }

        if (ChosenGuid.IsValid())
        {
            Anchors->BorrowedFuelGuid = ChosenGuid;
            // Same placement rule as an own (F); the fallback is right after the producer, because
            // the trigger fires with as little as two estimated laps in the tank and a tail stop
            // would demand the whole delivery lap plus the detour on that fuel.
            if (!NewRoute.Contains(ChosenGuid))
            {
                FuelPlacedIndex = FMath::Clamp(ChooseFuelInsertIndex(Vehicle, BaseLoop, ChosenGuid, 1), 1, NewRoute.Num());
                NewRoute.Insert(ChosenGuid, FuelPlacedIndex);
            }
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
        Anchors->EmergencyFuelGuid = PreviousEmergency;
        UE_LOG(LogDynamicTimeTableSubsystem, Warning,
            TEXT("DISPATCH_ROUTE_FAILED Truck='%s' Resource='%s' Trigger=%s"),
            *DTSVehicleName(Vehicle), *ResourceId, Trigger);
        return;
    }

    UE_LOG(LogDynamicTimeTableSubsystem, Verbose,
        TEXT("DISPATCH_ROUTE_REPLACED Truck='%s' Resource='%s' Requesters=%d Base=%s DedicatedFuel=%s FuelBorrowed=%s FuelIncluded=%s FuelIndex=%d RememberedFuelIndex=%d Stops=%d Trigger=%s"),
        *DTSVehicleName(Vehicle), *ResourceId, Requesters.Num(), BaseSource,
        bHasDedicatedFuel ? TEXT("true") : TEXT("false"),
        Anchors->BorrowedFuelGuid.IsValid() ? TEXT("true") : TEXT("false"),
        bFuelIncluded ? TEXT("true") : TEXT("false"), FuelPlacedIndex, Anchors->FuelInsertIndex, NewRoute.Num(), Trigger);
}

void UDynamicTimeTableSubsystem::ReleaseRequesterClaimIfDocked(AFGWheeledVehicleIdentifier* Vehicle, const FGuid& StationGuid)
{
    if (!IsValid(Vehicle)) return;
    FGuid DockGuid = StationGuid;
    if (!DockGuid.IsValid())
    {
        // Releasing Route[Index] here released the *next* requester's claim while the truck was
        // still loading at the producer, and another truck of the resource could claim that
        // station in the meantime.
        const TArray<FGuid>& Route = Vehicle->GetVehicleRoute();
        const int32 Index = Vehicle->GetCurrentTargetWaypointIndex();
        if (Route.Num() < 1 || !Route.IsValidIndex(Index)) return;
        DockGuid = Route[(Index - 1 + Route.Num()) % Route.Num()];
    }
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
        const bool bTimedOut = !ClaimedAt || Now - *ClaimedAt > Settings.RequesterClaimTimeoutSeconds;
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

    // A pool truck never enters the legacy planner: the pool writes its whole route.
    if (FDTTPoolTruckState* Pool = EnsurePoolMembership(Vehicle, Route, Trigger))
    {
        UpdatePoolVehicleName(Vehicle, *Pool);
        ProcessPoolTruckTarget(Vehicle, *Pool, Trigger);
        return;
    }

    const FString ResourceId = Route.Num() > 0
        ? ResolveDispatchResourceFromProducer(Route[0]) : FString();
    if (!ResourceId.IsEmpty())
    {
        // Dispatch and load-balancing are independent modes. Route[0], not the mutable display
        // name, opts this truck in and supplies the resource for every subsequent waypoint.
        UpdateDispatchVehicleName(Vehicle, ResourceId, Route, Index);
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
    UpdateRequesterWaitTimes();

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
        FDTTPoolTruckState* Pool = PoolTrucks.Find(Vehicle);
        const FString ResourceId = !Pool && Route.Num() > 0
            ? ResolveDispatchResourceFromProducer(Route[0]) : FString();
        if (Pool)
            UpdatePoolVehicleName(Vehicle, *Pool);
        else if (!ResourceId.IsEmpty())
            UpdateDispatchVehicleName(Vehicle, ResourceId, Route, TargetIndex);
        FDTTTruckPollState* Previous = TruckPollStates.Find(Vehicle);

        // First sighting only records. Acting here would redirect a vehicle that is already most
        // of the way to its target, every time the world loads. Joining the pool is recorded too:
        // a route with a depot in it says what the truck is, not where to send it right now.
        if (!Previous)
        {
            FDTTTruckPollState& Added = TruckPollStates.Add(Vehicle, FDTTTruckPollState{TargetIndex, bDocking});
            Added.LastRoute = Route;
            if (!Pool) EnsurePoolMembership(Vehicle, Route, TEXT("first sighting"));
            continue;
        }

        // A parked truck that the depot no longer reports as docked has been let go by something
        // this mod did not see (the hook did not fire, or the station was rebuilt). Its hold is
        // stale: drop it and let the truck drive its loop back to the depot.
        if (Pool && Pool->Phase == EDTTTruckPhase::Parked && bParkedTrucksRestored)
        {
            const AFGDockingStationIdentifier* Dock = FindDockByPathNode(Pool->DepotGuid);
            const AFGBuildableDockingStation* Depot = IsValid(Dock) ? Dock->GetStation() : nullptr;
            const AActor* Docked = IsValid(Depot) ? Depot->GetDockedActor() : nullptr;
            if (!IsValid(Depot) || Docked != Vehicle->GetOwnerVehicle() || !bDocking)
            {
                UE_LOG(LogDynamicTimeTableSubsystem, Warning, TEXT("PARK_LOST Truck='%s' Depot='%s' Docked='%s' Docking=%s"),
                    *DTSVehicleName(Vehicle), *DockDisplayName(Pool->DepotGuid), *GetNameSafe(Docked), bDocking ? TEXT("true") : TEXT("false"));
                UnparkTruck(Vehicle, *Pool, EDTTTruckPhase::ToDepot, TEXT("park lost"), false);
            }
        }

        // Arrival frees the slot, but does not re-evaluate: the target is still the dock the
        // vehicle is standing on, and rewriting that waypoint would send it away again.
        if (bDocking && !Previous->bDocking)
        {
            // With the docking hooks installed HandleTruckDocked has already marked the state
            // docked and this edge never shows; it remains as the fallback for a build where the
            // hooks did not install.
            UE_LOG(LogDynamicTimeTableSubsystem, Verbose, TEXT("DOCK_POLL Truck='%s' Index=%d Docking=true HooksInstalled=%s"),
                *DTSVehicleName(Vehicle), TargetIndex, FMyDTTHooks::AreDockingHooksInstalled() ? TEXT("true") : TEXT("false"));
            ReleaseTruckBooking(Vehicle, TEXT("truck docked"));
            ReleaseRequesterClaimIfDocked(Vehicle);
        }

        // The autopilot moving on to the next waypoint is the moment to substitute.
        if (TargetIndex != Previous->TargetIndex)
        {
            ProcessCurrentTruckTarget(Vehicle, TEXT("waypoint changed"));
        }
        else if (!bDocking && Previous->bDocking && TargetIndex > 0 &&
            (Pool ? Pool->Phase == EDTTTruckPhase::Assigned : !ResourceId.IsEmpty()))
        {
            // The index moves on at arrival, before the transfer (measured: docking at stop 1
            // already reports target 2), so "waypoint changed" always sees a full truck. Leaving
            // the dock is the first moment the cargo after an unload can be judged. Never at index
            // 0: that is the last stop of the lap, whose boundary already ran on arrival, and
            // running it again here re-claimed and rewrote the same lap five seconds later.
            ProcessCurrentTruckTarget(Vehicle, TEXT("undocked"));
        }
        else if (!ResourceId.IsEmpty() && Route.Num() < 2)
        {
            // A dispatch truck parked on a one-stop [Producer] route: its target index never
            // changes, so the branch above would never fire and the first request to open would go
            // unnoticed for good. Re-evaluate it on a slow timer instead.
            const UWorld* World = GetWorld();
            const double Now = IsValid(World) ? World->GetTimeSeconds() : 0.0;
            if (Now - Previous->LastIdleRecheckSeconds >= Settings.IdleDispatchRecheckSeconds)
            {
                Previous->LastIdleRecheckSeconds = Now;
                ProcessCurrentTruckTarget(Vehicle, TEXT("idle recheck"));
            }
        }

        // The index observed before the call, on purpose: a boundary is recognised only by seeing
        // the index change, so recording anything else here can hide the next one.
        Previous->TargetIndex = TargetIndex;
        Previous->bDocking = bDocking;
        Previous->LastRoute = Vehicle->GetVehicleRoute();
    }

    {
        const UWorld* World = GetWorld();
        const double Now = IsValid(World) ? World->GetTimeSeconds() : 0.0;
        TickLeftoverHolds(Now);
        TickDepotDrains(Now);
        // A request that opens while every truck is already parked has no boundary to be noticed at.
        if (PoolTrucks.Num() > 0 && Now - LastPoolDispatchSeconds >= Settings.PoolDispatchIntervalSeconds) RunPoolDispatch(TEXT("timer"));
    }
}

void UDynamicTimeTableSubsystem::HandleTruckDocked(AFGWheeledVehicle* Vehicle, AFGBuildableDockingStation* Station)
{
    if (!IsServer() || !IsValid(Vehicle)) return;
    AFGWheeledVehicleIdentifier* Identifier = Vehicle->GetVehicleIdentifier();
    if (!IsValid(Identifier)) return;

    const int32 TargetIndex = Identifier->GetCurrentTargetWaypointIndex();
    UE_LOG(LogDynamicTimeTableSubsystem, Log, TEXT("DOCK_EVENT Truck='%s' Station='%s' Index=%d Docking=true Autopilot=%s"),
        *DTSVehicleName(Identifier), IsValid(Station) ? *DTSDockName(Station->GetStationIdentifier()) : TEXT("<none>"),
        TargetIndex, Identifier->IsAutopilotEnabled() ? TEXT("on") : TEXT("off"));

    if (IsValid(Station) && IsValid(Station->GetStationIdentifier()) &&
        DispatchFuelStopGuids.Contains(Station->GetStationIdentifier()->GetPathNodeGUID()))
    {
        UFGInventoryComponent* FuelInventory = Vehicle->GetFuelInventory();
        FuelItemsAtDock.Add(Identifier, IsValid(FuelInventory) ? FuelInventory->GetNumItems(Identifier->GetFuelTypeDescriptor()) : 0);
    }

    FDTTTruckPollState* State = TruckPollStates.Find(Identifier);
    if (State)
    {
        State->DockedStation = Station;
        State->bDockedViaHook = true;
        // The poll's docking edge is this event, delivered late; marking the state docked here
        // keeps the poll from running the same edge again a second later.
        State->bDocking = true;
    }
    if (!Identifier->IsAutopilotEnabled()) return;

    ReleaseTruckBooking(Identifier, TEXT("truck docked"));
    {
        const AFGDockingStationIdentifier* DockedIdentifier = IsValid(Station) ? Station->GetStationIdentifier() : nullptr;
        ReleaseRequesterClaimIfDocked(Identifier, IsValid(DockedIdentifier) ? DockedIdentifier->GetPathNodeGUID() : FGuid());
    }

    // A pool truck docking at a depot parks there: at its own depot at the end of a lap, or at
    // any depot while it is looking for one. A lap in progress only parks at its own depot.
    if (IsValid(Station))
    {
        if (FDTTPoolTruckState* Pool = PoolTrucks.Find(Identifier))
        {
            const AFGDockingStationIdentifier* StationIdentifier = Station->GetStationIdentifier();
            const FGuid StationGuid = IsValid(StationIdentifier) ? StationIdentifier->GetPathNodeGUID() : FGuid();
            if (IsDepotGuid(StationGuid) && (Pool->Phase != EDTTTruckPhase::Assigned || StationGuid == Pool->DepotGuid))
            {
                ParkTruck(Identifier, *Pool, Station);
            }
        }
    }

    // The transfer at the end of this docking cycle moves what the lap planned, not what fits.
    // A plan is set only for a truck this mod manages; the game's own transfer runs otherwise.
    FDTTTransferPlan Plan;
    bool bLeftoverHold = false;
    if (IsValid(Station) && ComputeTransferPlan(Identifier, Station, Plan, &bLeftoverHold))
    {
        FMyDTTHooks::SetTransferPlan(Station, Plan);
        if (bLeftoverHold)
        {
            // The last requester of the lap with a hold configured: the station takes what it can
            // now (the Drain plan) and keeps taking once a second for a while, so the leftover does
            // not ride back to the depot. Without a hold the Drain plan alone is the whole story.
            FDTTLeftoverHold& Hold = LeftoverHolds.FindOrAdd(Identifier);
            Hold.Station = Station;
            Hold.ItemClass = Plan.ItemClass;
            Hold.bCycleDone = false;
            Hold.StartedSeconds = 0.0;
            Hold.LastTopUpSeconds = 0.0;
            Hold.MovedByTopUps = 0;
            FMyDTTHooks::SetHeld(Station, true, TEXT("leftover"));
            UE_LOG(LogDynamicTimeTableSubsystem, Display, TEXT("LEFTOVER_HOLD Truck='%s' Station='%s' Item=%s MaxSeconds=%.0f"),
                *DTSVehicleName(Identifier), *DTSDockName(Station->GetStationIdentifier()), *GetNameSafe(Plan.ItemClass), Settings.LeftoverHoldSeconds);
        }
    }
}

void UDynamicTimeTableSubsystem::HandleTruckUndocked(AFGWheeledVehicle* Vehicle)
{
    if (!IsServer() || !IsValid(Vehicle)) return;
    AFGWheeledVehicleIdentifier* Identifier = Vehicle->GetVehicleIdentifier();
    if (!IsValid(Identifier)) return;

    FDTTTruckPollState* State = TruckPollStates.Find(Identifier);
    AFGBuildableDockingStation* Station = State ? State->DockedStation.Get() : nullptr;
    const int32 TargetIndex = Identifier->GetCurrentTargetWaypointIndex();
    UE_LOG(LogDynamicTimeTableSubsystem, Log, TEXT("DOCK_EVENT Truck='%s' Station='%s' Index=%d Docking=false"),
        *DTSVehicleName(Identifier), IsValid(Station) ? *DTSDockName(Station->GetStationIdentifier()) : TEXT("<none>"), TargetIndex);

    if (Station) FMyDTTHooks::ClearTransferPlan(Station);
    int32 FuelBefore = 0;
    if (FuelItemsAtDock.RemoveAndCopyValue(Identifier, FuelBefore))
    {
        UFGInventoryComponent* FuelInventory = Vehicle->GetFuelInventory();
        const int32 FuelAfter = IsValid(FuelInventory) ? FuelInventory->GetNumItems(Identifier->GetFuelTypeDescriptor()) : 0;
        const FString StationName = IsValid(Station) ? DTSDockName(Station->GetStationIdentifier()) : TEXT("<none>");
        if (FuelAfter > FuelBefore)
        {
            UE_LOG(LogDynamicTimeTableSubsystem, Display, TEXT("FUEL_REFUELLED Truck='%s' Station='%s' Fuel=%d->%d"),
                *DTSVehicleName(Identifier), *StationName, FuelBefore, FuelAfter);
        }
        else
        {
            // The (F) stop had nothing to give (empty, unpowered, wrong fuel): the truck leaves
            // with what it came with and will strand if this keeps up.
            UFGInventoryComponent* StationInventory = IsValid(Station) ? Station->GetInventory() : nullptr;
            UE_LOG(LogDynamicTimeTableSubsystem, Warning, TEXT("FUEL_STOP_EMPTY Truck='%s' Station='%s' Fuel=%d StationItems=%d"),
                *DTSVehicleName(Identifier), *StationName, FuelAfter,
                IsValid(StationInventory) ? StationInventory->GetNumItems(nullptr) : -1);
        }
    }
    if (LeftoverHolds.Contains(Identifier)) ReleaseLeftoverHold(Identifier, TEXT("undocked"), false);
    // A release drops the phase before it undocks, so a truck still marked parked here was
    // undocked behind this mod's back. The hold is gone with the docking; only the state is stale.
    if (FDTTPoolTruckState* Pool = PoolTrucks.Find(Identifier))
    {
        if (Pool->Phase == EDTTTruckPhase::Parked)
        {
            UE_LOG(LogDynamicTimeTableSubsystem, Warning, TEXT("PARK_LOST Truck='%s' Depot='%s' Reason=undocked"),
                *DTSVehicleName(Identifier), *DockDisplayName(Pool->DepotGuid));
            UnparkTruck(Identifier, *Pool, EDTTTruckPhase::ToDepot, TEXT("undocked"), false);
        }
    }

    if (!State) return;
    State->DockedStation = nullptr;

    // The poll's "undocked" branch, run now instead of up to a second later. Marking the state
    // as no longer docking keeps the poll from running it a second time; the target index is
    // left alone, since a boundary is recognised only by the poll seeing the index change.
    const bool bWasDocking = State->bDocking;
    State->bDocking = false;
    if (!bWasDocking || !Identifier->IsAutopilotEnabled()) return;
    const TArray<FGuid>& Route = Identifier->GetVehicleRoute();
    const FDTTPoolTruckState* Pool = PoolTrucks.Find(Identifier);
    const bool bManagedLap = Pool
        ? Pool->Phase == EDTTTruckPhase::Assigned
        : Route.Num() > 0 && !ResolveDispatchResourceFromProducer(Route[0]).IsEmpty();
    if (bManagedLap && TargetIndex > 0 && TargetIndex == State->TargetIndex)
    {
        ProcessCurrentTruckTarget(Identifier, TEXT("undocked"));
    }

    // The last requester of a pool lap is where the next lap can start. The dispatcher gets a look
    // at this truck now, from here, instead of after the drive back to the depot; a request that
    // opens while it is still driving back finds it on the timer the same way.
    if (FDTTPoolTruckState* PoolState = PoolTrucks.Find(Identifier))
    {
        const AFGDockingStationIdentifier* Dock = IsValid(Station) ? Station->GetStationIdentifier() : nullptr;
        const FGuid StationGuid = IsValid(Dock) ? Dock->GetPathNodeGUID() : FGuid();
        if (PoolState->Phase == EDTTTruckPhase::Assigned && StationGuid.IsValid() &&
            PoolState->Requesters.Num() > 0 && PoolState->Requesters.Last() == StationGuid)
        {
            PoolState->ChainFromGuid = StationGuid;
            UE_LOG(LogDynamicTimeTableSubsystem, Display, TEXT("POOL_LAP_DONE Truck='%s' LastRequester='%s' Depot='%s'"),
                *DTSVehicleName(Identifier), *DockDisplayName(StationGuid), *DockDisplayName(PoolState->DepotGuid));
            RunPoolDispatch(TEXT("lap end"));
        }
    }
}

void UDynamicTimeTableSubsystem::HandleHeldStationEvicted(AFGBuildableDockingStation* Station, AActor* NewActor)
{
    if (!IsServer() || !IsValid(Station)) return;
    // A leftover drain evicted by the next truck: the hook dropped the hold, the game undocks
    // ours next; only the bookkeeping is left.
    for (auto It = LeftoverHolds.CreateIterator(); It; ++It)
    {
        if (It.Value().Station.Get() != Station) continue;
        AFGWheeledVehicleIdentifier* Vehicle = It.Key().Get();
        UE_LOG(LogDynamicTimeTableSubsystem, Display, TEXT("LEFTOVER_RELEASED Truck='%s' Station='%s' Reason=evicted by '%s' Moved=%d"),
            *DTSVehicleName(Vehicle), *DTSDockName(Station->GetStationIdentifier()), *GetNameSafe(NewActor), It.Value().MovedByTopUps);
        It.RemoveCurrent();
        return;
    }
    UE_LOG(LogDynamicTimeTableSubsystem, Warning, TEXT("DEPOT_EVICTED Station='%s' Parked='%s' By='%s'"),
        *DTSDockName(Station->GetStationIdentifier()), *GetNameSafe(Station->GetDockedActor()), *GetNameSafe(NewActor));
    const AFGDockingStationIdentifier* StationIdentifier = Station->GetStationIdentifier();
    const FGuid StationGuid = IsValid(StationIdentifier) ? StationIdentifier->GetPathNodeGUID() : FGuid();
    if (!StationGuid.IsValid()) return;

    // The hook has already dropped the hold; the game undocks the truck next. It keeps its claim
    // and its [Depot, Depot] loop, so it comes back and docks again once the intruder has left -
    // or evicts the intruder itself, the way it was just evicted.
    for (auto& Pair : PoolTrucks)
    {
        AFGWheeledVehicleIdentifier* Vehicle = Pair.Key.Get();
        FDTTPoolTruckState& State = Pair.Value;
        if (!IsValid(Vehicle) || State.Phase != EDTTTruckPhase::Parked || State.DepotGuid != StationGuid) continue;
        UnparkTruck(Vehicle, State, EDTTTruckPhase::ToDepot, TEXT("evicted"), false);
    }
}

void UDynamicTimeTableSubsystem::HandleVehicleRouteChanged(AFGWheeledVehicleIdentifier* Vehicle)
{
    if (!IsServer() || !IsValid(Vehicle) || InternalRouteChanges.Contains(Vehicle)) return;
    // The game calls SetVehicleRoute with the unchanged route on its own - measured three times
    // per docking. Only a route that actually differs from the one last seen is a player edit;
    // treating the re-saves as edits dropped the other claims of a multi-stop lap at the first
    // unload and marked every lap as hand-edited.
    if (const FDTTTruckPollState* Seen = TruckPollStates.Find(Vehicle))
    {
        if (Seen->LastRoute == Vehicle->GetVehicleRoute())
        {
            UE_LOG(LogDynamicTimeTableSubsystem, Verbose, TEXT("ROUTE_RESAVED_UNCHANGED Truck='%s' Waypoints=%d"),
                *DTSVehicleName(Vehicle), Vehicle->GetVehicleRoute().Num());
            return;
        }
        // The route that was there before this mod's last write is back: the game wrote its own
        // copy back at the end of a docking that had begun before the write (a lap is rebuilt on
        // arrival, while docking). Not an edit - restore the lap. Nothing else is touched: the
        // claims are still right, and adopting an (F) out of that stale route would be wrong.
        if (Seen->LastWrittenRoute.Num() >= 2 && Seen->LastWrittenRoute != Seen->RouteBeforeLastWrite &&
            Seen->RouteBeforeLastWrite == Vehicle->GetVehicleRoute())
        {
            const TArray<FGuid> Restore = Seen->LastWrittenRoute;
            const bool bRestored = ReplaceRoute(Vehicle, Restore);
            UE_LOG(LogDynamicTimeTableSubsystem, Display, TEXT("ROUTE_REVERTED_BY_GAME Truck='%s' Waypoints=%d Restored=%s Stops=%d"),
                *DTSVehicleName(Vehicle), Vehicle->GetVehicleRoute().Num(), bRestored ? TEXT("true") : TEXT("false"), Restore.Num());
            return;
        }
    }
    ReleaseTruckBooking(Vehicle, TEXT("route saved"));
    ReleaseRequesterClaimsForVehicle(Vehicle, TEXT("route saved"));
    if (LeftoverHolds.Contains(Vehicle)) ReleaseLeftoverHold(Vehicle, TEXT("route saved by player"), true);
    // A pool truck whose saved route has no depot left is the player's again. One that still has
    // a depot runs the player's route until it docks at a depot, where it parks and rejoins.
    if (FDTTPoolTruckState* Pool = PoolTrucks.Find(Vehicle))
    {
        if (!RouteHasDepot(Vehicle->GetVehicleRoute()))
        {
            LeavePool(Vehicle, TEXT("route saved without a depot"));
        }
        else if (Pool->Phase == EDTTTruckPhase::Parked)
        {
            UnparkTruck(Vehicle, *Pool, EDTTTruckPhase::Manual, TEXT("route saved by player"), true);
        }
        else if (Pool->Phase != EDTTTruckPhase::Manual)
        {
            Pool->Phase = EDTTTruckPhase::Manual;
            UE_LOG(LogDynamicTimeTableSubsystem, Display, TEXT("POOL_MANUAL Truck='%s' Reason=route saved by player"), *DTSVehicleName(Vehicle));
        }
    }
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
    if (!bEnabled && LeftoverHolds.Contains(Vehicle)) ReleaseLeftoverHold(Vehicle, TEXT("autopilot disabled"), true);
    // A pool truck with the autopilot off is the player's: the hold goes, the depot claim goes,
    // the membership stays so that it parks again once it is switched back on and reaches a depot.
    if (FDTTPoolTruckState* Pool = PoolTrucks.Find(Vehicle))
    {
        if (!bEnabled)
        {
            if (Pool->Phase == EDTTTruckPhase::Parked) UnparkTruck(Vehicle, *Pool, EDTTTruckPhase::Manual, TEXT("autopilot disabled"), true);
            else Pool->Phase = EDTTTruckPhase::Manual;
            ReleaseDepotClaim(Vehicle, TEXT("autopilot disabled"));
            Pool->DepotGuid.Invalidate();
            UE_LOG(LogDynamicTimeTableSubsystem, Display, TEXT("POOL_MANUAL Truck='%s' Reason=autopilot disabled"), *DTSVehicleName(Vehicle));
        }
        else
        {
            Pool->Phase = EDTTTruckPhase::ToDepot;
        }
    }
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
        WeakThis->RestoreParkedTrucks();
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
        // Fluids included: fluid trucks and fluid truck stations exist, and without this a
        // "(Crude Oil)" tag could only ever resolve through the producer-cargo fallback.
        UFGBlueprintFunctionLibrary::GetAllDescriptorsSorted(const_cast<UDynamicTimeTableSubsystem*>(this), All,
            /*includeFluids*/ true, /*includeBuildings*/ false, /*onlyUnlocked*/ false);
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

    // A name the game does not know (a player's own word for the item) is resolved from what the
    // tagged stations hold: the producers first, and when no producer of this resource holds
    // anything - none tagged, or all empty - the requesters, so a pool truck can still be sent
    // from its own cargo. Either way the answer counts only when it is unambiguous.
    TSet<TSubclassOf<UFGItemDescriptor>> CargoClasses;
    for (const EDTTDispatchRole Role : { EDTTDispatchRole::Producer, EDTTDispatchRole::Requester })
    {
        for (const FDTTDispatchStation& Station : DispatchStations)
        {
            if (GetEffectiveDispatchRole(Station) != Role ||
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
                if (Stack.HasItems()) CargoClasses.Add(Stack.Item.GetItemClass());
            }
        }
        if (CargoClasses.Num() > 0) break;
    }

    if (CargoClasses.Num() != 1) return nullptr;
    return *CargoClasses.CreateConstIterator();
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

const FDTTDispatchStation* UDynamicTimeTableSubsystem::FindDispatchStationByGuid(const FGuid& PathNodeGuid) const
{
    if (!PathNodeGuid.IsValid()) return nullptr;
    return DispatchStations.FindByPredicate([&](const FDTTDispatchStation& Station)
    {
        return Station.DockingPathNodeGuid == PathNodeGuid;
    });
}

bool UDynamicTimeTableSubsystem::ComputeTransferPlan(
    AFGWheeledVehicleIdentifier* Vehicle, AFGBuildableDockingStation* Station, FDTTTransferPlan& OutPlan, bool* bOutLeftoverHold) const
{
    OutPlan = FDTTTransferPlan();
    if (bOutLeftoverHold) *bOutLeftoverHold = false;
    if (!IsValid(Vehicle) || !IsValid(Station)) return false;
    const AFGDockingStationIdentifier* StationIdentifier = Station->GetStationIdentifier();
    if (!IsValid(StationIdentifier)) return false;
    const FGuid StationGuid = StationIdentifier->GetPathNodeGUID();

    // A depot in UNLOAD mode is where leftovers end up: everything on board goes into it, now and
    // (TickDepotDrains) while the truck stays parked. A depot in LOAD mode is a waiting place only.
    if (PoolTrucks.Contains(Vehicle) && IsDepotGuid(StationGuid))
    {
        const bool bDrain = Settings.bUnloadAtDepot && !Station->GetIsInLoadMode();
        OutPlan.Mode = bDrain ? FDTTTransferPlan::EMode::Drain : FDTTTransferPlan::EMode::Block;
        UE_LOG(LogDynamicTimeTableSubsystem, Log, TEXT("XFER_PLAN Truck='%s' Station='%s' Mode=%s Reason=%s"),
            *DTSVehicleName(Vehicle), *DTSDockName(StationIdentifier), bDrain ? TEXT("Drain") : TEXT("Block"),
            bDrain ? TEXT("depot unload") : (Settings.bUnloadAtDepot ? TEXT("depot in LOAD mode") : TEXT("depot unload disabled")));
        return true;
    }

    // Exact quantities off (the default): every other docking runs the game's own transfer - the
    // producer fills the truck, a requester takes everything that fits - and no plan is set at
    // all, so no drain and no hold either. The depot above keeps taking leftovers regardless.
    if (!Settings.bExactQuantities)
    {
        UE_LOG(LogDynamicTimeTableSubsystem, Log, TEXT("XFER_PLAN Truck='%s' Station='%s' Mode=PassThrough Reason=exact quantities off"),
            *DTSVehicleName(Vehicle), *DTSDockName(StationIdentifier));
        return false;
    }

    // A pool lap knows its resource and producer from its state: its route may start with the
    // depot it was released from (see AssignPoolTruck), so Route[0] says nothing.
    const TArray<FGuid>& Route = Vehicle->GetVehicleRoute();
    const FDTTPoolTruckState* Pool = PoolTrucks.Find(Vehicle);
    FString ResourceId;
    FGuid ProducerGuid;
    if (Pool)
    {
        if (Pool->Phase != EDTTTruckPhase::Assigned) return false;
        ResourceId = Pool->ResourceId;
        ProducerGuid = Pool->ProducerGuid;
    }
    else
    {
        ResourceId = Route.Num() > 0 ? ResolveDispatchResourceFromProducer(Route[0]) : FString();
        ProducerGuid = Route.Num() > 0 ? Route[0] : FGuid();
    }
    // A pool lap served from the truck's own cargo has no producer at all.
    if (ResourceId.IsEmpty() || (!Pool && !ProducerGuid.IsValid())) return false;

    const TSubclassOf<UFGItemDescriptor> ItemClass = ResolveItemClassForResource(ResourceId);
    if (!ItemClass)
    {
        UE_LOG(LogDynamicTimeTableSubsystem, Warning, TEXT("XFER_PLAN Truck='%s' Station='%s' Mode=PassThrough Reason=item-class-not-found Resource='%s'"),
            *DTSVehicleName(Vehicle), *DTSDockName(StationIdentifier), *ResourceId);
        return true;
    }
    const int64 StackSize = FMath::Max(1, UFGItemDescriptor::GetStackSize(ItemClass));
    const TCHAR* Reason = TEXT("other stop");

    if (StationGuid == ProducerGuid && Station->GetIsInLoadMode())
    {
        // The producer. Load only what the requesters of this lap can take: each one's deficit,
        // read now, on top of what is already on board. A lap without a requester (no demand, the
        // last consumer kept for the round trip) loads as the game would.
        int64 DeficitItems = 0;
        int32 Requesters = 0;
        for (int32 Index = 0; Index < Route.Num(); ++Index)
        {
            if (Route[Index] == ProducerGuid) continue;
            const FDTTDispatchStation* Stop = FindDispatchStationByGuid(Route[Index]);
            if (!Stop || !Stop->ResourceId.Equals(ResourceId, ESearchCase::IgnoreCase) ||
                GetEffectiveDispatchRole(*Stop) != EDTTDispatchRole::Requester || !DoesDispatchStationMatchMode(*Stop))
            {
                continue;
            }
            FDTTRequestLevels Levels;
            if (!GetRequestLevels(*Stop, ItemClass, Levels)) continue;
            const int32 Stock = ReadDockStockInStacks(Stop->DockingStationIdentifier.Get(), ItemClass);
            DeficitItems += int64(FMath::Max(0, Levels.Target - Stock)) * StackSize;
            ++Requesters;
        }
        if (Requesters > 0)
        {
            AFGWheeledVehicle* WheeledVehicle = Vehicle->GetOwnerVehicle();
            UFGInventoryComponent* Storage = IsValid(WheeledVehicle) ? WheeledVehicle->GetStorageInventory() : nullptr;
            const int64 OnBoard = IsValid(Storage) ? Storage->GetNumItems(ItemClass) : 0;
            int64 CapacityItems = TNumericLimits<int32>::Max();
            if (IsValid(Storage) && Storage->GetSizeLinear() > 0)
            {
                CapacityItems = int64(Storage->GetSizeLinear()) * FMath::Max<int64>(1, Storage->GetSlotSize(0, ItemClass));
            }
            OutPlan.Mode = FDTTTransferPlan::EMode::FillTo;
            OutPlan.ItemClass = ItemClass;
            // A fluid truck under "fluid full load" leaves with a full tank: the one requester of
            // its lap drains it, and what the tank holds beyond the deficit is what the consumer
            // drinks while the truck is held there.
            const bool bFullLoad = Settings.bFluidFullLoad && IsFluidTruck(Vehicle);
            const int64 Wanted = bFullLoad ? CapacityItems : OnBoard + DeficitItems;
            OutPlan.TargetItems = int32(FMath::Min<int64>(Wanted, FMath::Min<int64>(CapacityItems, TNumericLimits<int32>::Max())));
            Reason = bFullLoad ? TEXT("producer full load") : TEXT("producer");
        }
        else
        {
            Reason = TEXT("producer without requesters");
        }
    }
    else if (const FDTTDispatchStation* Stop = FindDispatchStationByGuid(StationGuid))
    {
        if (Stop->ResourceId.Equals(ResourceId, ESearchCase::IgnoreCase) &&
            GetEffectiveDispatchRole(*Stop) == EDTTDispatchRole::Requester && DoesDispatchStationMatchMode(*Stop))
        {
            // A requester: fill it up to its target (limit plus buffer) and not beyond, so the
            // truck carries the rest on to the next stop.
            FDTTRequestLevels Levels;
            if (GetRequestLevels(*Stop, ItemClass, Levels))
            {
                OutPlan.Mode = FDTTTransferPlan::EMode::FillTo;
                OutPlan.ItemClass = ItemClass;
                OutPlan.TargetItems = int32(FMath::Min<int64>(int64(Levels.Target) * StackSize, TNumericLimits<int32>::Max()));
                Reason = TEXT("requester");
                // The last planned stop of the lap takes everything it can in its docking cycle,
                // target or not: what stays on board here rides to the depot otherwise. Off
                // (DrainLeftovers), the truck keeps its leftovers for the next lap. The hold is a
                // separate option on top (LeftoverHoldSeconds > 0, off by default since 2.4):
                // HandleTruckDocked then keeps the truck docked and tops the station up as it
                // consumes. Before 2.4 a hold of 0 switched the drain off with it.
                // A fluid truck under "fluid full load" leaves the producer with a full tank on every
                // lap, planned or idle (the producer counts the requesters of the route, not of the
                // plan), so its last requester in route order drains it even when the lap planned
                // nothing - otherwise the leftover rides the loop and the consumer is topped up by
                // one fill per lap instead of being held (seen 13.09.2026 on the water truck).
                const bool bFullLoadLastStop = Settings.bFluidFullLoad && !PoolTrucks.Contains(Vehicle) &&
                    IsFluidTruck(Vehicle) && IsLastRequesterInRoute(Vehicle, StationGuid, ResourceId);
                if (Settings.bDrainLeftovers && !Station->GetIsInLoadMode() &&
                    (bFullLoadLastStop || IsLastPlannedRequester(Vehicle, StationGuid)))
                {
                    OutPlan.Mode = FDTTTransferPlan::EMode::Drain;
                    Reason = TEXT("last requester");
                    if (bOutLeftoverHold) *bOutLeftoverHold = Settings.LeftoverHoldSeconds > 0.0f;
                }
            }
            else
            {
                Reason = TEXT("requester capacity unreadable");
            }
        }
    }

    UE_LOG(LogDynamicTimeTableSubsystem, Log, TEXT("XFER_PLAN Truck='%s' Station='%s' Mode=%s Item=%s Target=%d Reason=%s"),
        *DTSVehicleName(Vehicle), *DTSDockName(StationIdentifier),
        OutPlan.Mode == FDTTTransferPlan::EMode::FillTo ? TEXT("FillTo") : OutPlan.Mode == FDTTTransferPlan::EMode::Drain ? TEXT("Drain")
            : OutPlan.Mode == FDTTTransferPlan::EMode::Block ? TEXT("Block") : TEXT("PassThrough"),
        *ItemClass->GetName(), OutPlan.TargetItems, Reason);
    return true;
}

bool UDynamicTimeTableSubsystem::IsLastPlannedRequester(AFGWheeledVehicleIdentifier* Vehicle, const FGuid& StationGuid) const
{
    if (!IsValid(Vehicle) || !StationGuid.IsValid()) return false;
    if (const FDTTPoolTruckState* Pool = PoolTrucks.Find(Vehicle))
    {
        return Pool->Phase == EDTTTruckPhase::Assigned && Pool->Requesters.Num() > 0 && Pool->Requesters.Last() == StationGuid;
    }
    // A legacy lap: the last stop in route order that this lap planned something for. The idle
    // [Producer, LastConsumer] loop plans nothing, so it never drains - it would fill the consumer
    // past its threshold every lap.
    const FDTTDispatchTruckAnchors* Anchors = DispatchTruckAnchors.Find(Vehicle);
    if (!Anchors || Anchors->PlannedItems.Num() == 0) return false;
    const TArray<FGuid>& Route = Vehicle->GetVehicleRoute();
    for (int32 Index = Route.Num() - 1; Index >= 0; --Index)
    {
        if (Anchors->PlannedItems.Contains(Route[Index])) return Route[Index] == StationGuid;
    }
    return false;
}

bool UDynamicTimeTableSubsystem::IsLastRequesterInRoute(AFGWheeledVehicleIdentifier* Vehicle, const FGuid& StationGuid, const FString& ResourceId) const
{
    if (!IsValid(Vehicle) || !StationGuid.IsValid()) return false;
    const TArray<FGuid>& Route = Vehicle->GetVehicleRoute();
    for (int32 Index = Route.Num() - 1; Index >= 0; --Index)
    {
        const FDTTDispatchStation* Stop = FindDispatchStationByGuid(Route[Index]);
        if (!Stop || !Stop->ResourceId.Equals(ResourceId, ESearchCase::IgnoreCase) ||
            GetEffectiveDispatchRole(*Stop) != EDTTDispatchRole::Requester || !DoesDispatchStationMatchMode(*Stop))
        {
            continue;
        }
        return Route[Index] == StationGuid;
    }
    return false;
}

bool UDynamicTimeTableSubsystem::IsCargoDrainedAtDepot(const FDTTPoolTruckState& State) const
{
    if (State.Phase != EDTTTruckPhase::Parked || !Settings.bUnloadAtDepot) return false;
    const AFGDockingStationIdentifier* Dock = FindDockByPathNode(State.DepotGuid);
    const AFGBuildableDockingStation* Station = IsValid(Dock) ? Dock->GetStation() : nullptr;
    return IsValid(Station) && !Station->GetIsInLoadMode();
}

int32 UDynamicTimeTableSubsystem::OnBoardStacksOf(AFGWheeledVehicleIdentifier* Vehicle, TSubclassOf<UFGItemDescriptor> ItemClass) const
{
    int32 Capacity = 0, OnBoard = 0;
    return EstimateTruckCargoStacks(Vehicle, ItemClass, Capacity, OnBoard) ? OnBoard : 0;
}

FString UDynamicTimeTableSubsystem::DescribeCargo(AFGWheeledVehicleIdentifier* Vehicle, TSubclassOf<UFGItemDescriptor>* OutSingleClass) const
{
    if (OutSingleClass) *OutSingleClass = nullptr;
    AFGWheeledVehicle* WheeledVehicle = IsValid(Vehicle) ? Vehicle->GetOwnerVehicle() : nullptr;
    UFGInventoryComponent* Storage = IsValid(WheeledVehicle) ? WheeledVehicle->GetStorageInventory() : nullptr;
    if (!IsValid(Storage)) return FString();
    TArray<FInventoryStack> Stacks;
    Storage->GetInventoryStacks(Stacks);
    TMap<TSubclassOf<UFGItemDescriptor>, int64> Items;
    for (const FInventoryStack& Stack : Stacks)
    {
        if (Stack.HasItems()) Items.FindOrAdd(Stack.Item.GetItemClass()) += Stack.NumItems;
    }
    if (Items.Num() == 1 && OutSingleClass) *OutSingleClass = Items.CreateConstIterator().Key();
    TArray<FString> Parts;
    for (const auto& Pair : Items)
    {
        // Whole stacks, rounded down like EstimateTruckCargoStacks, so the note and the dispatcher's
        // OnBoard agree; the item count says what the rounding dropped.
        const int64 StackSize = FMath::Max(1, UFGItemDescriptor::GetStackSize(Pair.Key));
        Parts.Add(FString::Printf(TEXT("%s %lld (%lld stacks)"), *UFGItemDescriptor::GetItemName(Pair.Key).ToString(), Pair.Value, Pair.Value / StackSize));
    }
    Parts.Sort();
    return FString::Join(Parts, TEXT(", "));
}

void UDynamicTimeTableSubsystem::TickLeftoverHolds(double Now)
{
    // Releases remove entries, so they are collected and done after the walk.
    TArray<TPair<AFGWheeledVehicleIdentifier*, const TCHAR*>> Releases;
    for (auto It = LeftoverHolds.CreateIterator(); It; ++It)
    {
        AFGWheeledVehicleIdentifier* Vehicle = It.Key().Get();
        FDTTLeftoverHold& Hold = It.Value();
        AFGBuildableDockingStation* Station = Hold.Station.Get();
        AFGWheeledVehicle* Owner = IsValid(Vehicle) ? Vehicle->GetOwnerVehicle() : nullptr;
        if (!IsValid(Vehicle) || !IsValid(Station) || !IsValid(Owner) || Station->GetDockedActor() != Owner)
        {
            if (IsValid(Station)) FMyDTTHooks::SetHeld(Station, false, TEXT("leftover hold lost"));
            UE_LOG(LogDynamicTimeTableSubsystem, Warning, TEXT("LEFTOVER_LOST Truck='%s' Station='%s' Moved=%d"),
                *DTSVehicleName(Vehicle), IsValid(Station) ? *DTSDockName(Station->GetStationIdentifier()) : TEXT("<none>"), Hold.MovedByTopUps);
            It.RemoveCurrent();
            continue;
        }
        // The docking cycle's own transfer runs at its end; the clock starts once it has.
        if (Station->IsLoadUnloading()) continue;
        if (!Hold.bCycleDone)
        {
            Hold.bCycleDone = true;
            Hold.StartedSeconds = Now;
            Hold.LastTopUpSeconds = Now;
        }
        UFGInventoryComponent* Storage = Owner->GetStorageInventory();
        const int32 Left = IsValid(Storage) && Hold.ItemClass ? Storage->GetNumItems(Hold.ItemClass) : 0;
        if (Left <= 0)
        {
            Releases.Emplace(Vehicle, TEXT("empty"));
            continue;
        }
        if (Now - Hold.StartedSeconds >= Settings.LeftoverHoldSeconds)
        {
            Releases.Emplace(Vehicle, TEXT("timeout"));
            continue;
        }
        if (Now - Hold.LastTopUpSeconds >= Settings.TopUpIntervalSeconds)
        {
            Hold.LastTopUpSeconds = Now;
            FDTTTransferPlan Plan;
            Plan.Mode = FDTTTransferPlan::EMode::Drain;
            Plan.ItemClass = Hold.ItemClass;
            Hold.MovedByTopUps += FMyDTTHooks::TransferNow(Station, Storage, Plan, true);
        }
    }
    for (const auto& Release : Releases) ReleaseLeftoverHold(Release.Key, Release.Value, true);
}

void UDynamicTimeTableSubsystem::ReleaseLeftoverHold(AFGWheeledVehicleIdentifier* Vehicle, const TCHAR* Reason, bool bUndock)
{
    FDTTLeftoverHold Hold;
    if (!LeftoverHolds.RemoveAndCopyValue(Vehicle, Hold)) return;
    AFGBuildableDockingStation* Station = Hold.Station.Get();
    AFGWheeledVehicle* Owner = IsValid(Vehicle) ? Vehicle->GetOwnerVehicle() : nullptr;
    UFGInventoryComponent* Storage = IsValid(Owner) ? Owner->GetStorageInventory() : nullptr;
    const int32 Left = IsValid(Storage) && Hold.ItemClass ? Storage->GetNumItems(Hold.ItemClass) : -1;
    const UWorld* World = GetWorld();
    const double Held = Hold.bCycleDone && IsValid(World) ? World->GetTimeSeconds() - Hold.StartedSeconds : 0.0;
    if (IsValid(Station))
    {
        FMyDTTHooks::SetHeld(Station, false, Reason);
        if (bUndock && IsValid(Owner) && Station->GetDockedActor() == Owner && !Station->IsLoadUnloading()) Station->ForceUndockActor();
    }
    UE_LOG(LogDynamicTimeTableSubsystem, Display, TEXT("LEFTOVER_RELEASED Truck='%s' Station='%s' Reason=%s Moved=%d Left=%d Seconds=%.0f"),
        *DTSVehicleName(Vehicle), IsValid(Station) ? *DTSDockName(Station->GetStationIdentifier()) : TEXT("<none>"), Reason,
        Hold.MovedByTopUps, Left, Held);
}

void UDynamicTimeTableSubsystem::TickDepotDrains(double Now)
{
    if (!Settings.bUnloadAtDepot || Now - LastDepotTopUpSeconds < Settings.TopUpIntervalSeconds) return;
    LastDepotTopUpSeconds = Now;
    for (auto& Pair : PoolTrucks)
    {
        AFGWheeledVehicleIdentifier* Vehicle = Pair.Key.Get();
        const FDTTPoolTruckState& State = Pair.Value;
        if (!IsValid(Vehicle) || State.Phase != EDTTTruckPhase::Parked) continue;
        const AFGDockingStationIdentifier* Dock = FindDockByPathNode(State.DepotGuid);
        AFGBuildableDockingStation* Station = IsValid(Dock) ? Dock->GetStation() : nullptr;
        AFGWheeledVehicle* Owner = Vehicle->GetOwnerVehicle();
        if (!IsValid(Station) || !IsValid(Owner) || Station->GetDockedActor() != Owner || Station->GetIsInLoadMode()) continue;
        UFGInventoryComponent* Storage = Owner->GetStorageInventory();
        if (!IsValid(Storage) || Storage->IsEmpty()) continue;
        FDTTTransferPlan Plan;
        Plan.Mode = FDTTTransferPlan::EMode::Drain;
        FMyDTTHooks::TransferNow(Station, Storage, Plan, true);
    }
}

bool UDynamicTimeTableSubsystem::ApplySettings(const FDTTSettings& NewSettings, const TCHAR* Source, FString* OutMessage)
{
    if (!IsServer())
    {
        if (OutMessage) *OutMessage = TEXT("dTT: only the server changes settings.");
        return false;
    }
    FDTTSettings Clean = NewSettings;
    Clean.Sanitize();
    if (Clean == Settings)
    {
        if (OutMessage) *OutMessage = TEXT("dTT: settings unchanged.");
        return false;
    }
    Settings = Clean;
    ++ConfigurationRevision;
    SyncConfigurationToSaveActor();
    UE_LOG(LogDynamicTimeTableSubsystem, Display, TEXT("SETTINGS_APPLIED Source=%s %s"), Source, *Settings.Describe());
    if (OutMessage) *OutMessage = FString::Printf(TEXT("dTT: settings applied: %s"), *Settings.Describe());
    OnGroupsChanged.Broadcast(); OnGroupsChangedNative.Broadcast();
    return true;
}

bool UDynamicTimeTableSubsystem::ReadDockCapacityInStacks(
    AFGDockingStationIdentifier* Dock, TSubclassOf<UFGItemDescriptor> ItemClass, int32& OutCapacityStacks) const
{
    OutCapacityStacks = INDEX_NONE;
    if (!IsValid(Dock) || !ItemClass) return false;
    AFGBuildableDockingStation* Building = Dock->GetStation();
    UFGInventoryComponent* Storage = IsValid(Building) ? Building->GetInventory() : nullptr;
    if (!IsValid(Storage) || Storage->GetSizeLinear() < 1) return false;
    const int64 StackSize = FMath::Max(1, UFGItemDescriptor::GetStackSize(ItemClass));
    const int64 SlotSize = FMath::Max<int64>(1, Storage->GetSlotSize(0, ItemClass));
    OutCapacityStacks = int32(FMath::Max<int64>(1, (Storage->GetSizeLinear() * SlotSize) / StackSize));
    return true;
}

// ---------------------------------------------------------------------------
// Shared pool. A truck whose route carries a "(D)" depot belongs to the pool: it drives to a
// depot on a [Depot, Depot] loop, parks there held docked, and (milestone 4) is sent on a lap
// when a requester opens. Everything here runs on the game thread; the hold itself lives in
// FMyDTTHooks and is only switched on and off from here.
// ---------------------------------------------------------------------------

bool UDynamicTimeTableSubsystem::RouteHasDepot(const TArray<FGuid>& Route) const
{
    if (DispatchDepotGuids.Num() == 0) return false;
    for (const FGuid& Node : Route) if (DispatchDepotGuids.Contains(Node)) return true;
    return false;
}

FString UDynamicTimeTableSubsystem::DockDisplayName(const FGuid& PathNodeGuid) const
{
    const AFGDockingStationIdentifier* Dock = FindDockByPathNode(PathNodeGuid);
    if (!IsValid(Dock)) return PathNodeGuid.IsValid() ? PathNodeGuid.ToString().Left(8) : FString(TEXT("?"));
    return StripAllTags(Dock->GetStationName().ToString());
}

FDTTPoolTruckState& UDynamicTimeTableSubsystem::JoinPool(
    AFGWheeledVehicleIdentifier* Vehicle, const FGuid& DepotGuid, const TCHAR* Reason)
{
    // The pool writes this truck's whole route from now on; the legacy planner's bookkeeping for
    // it would only fight that.
    ReleaseTruckBooking(Vehicle, TEXT("joined pool"));
    ReleaseRequesterClaimsForVehicle(Vehicle, TEXT("joined pool"));
    DispatchTruckAnchors.Remove(Vehicle);

    FDTTPoolTruckState& State = PoolTrucks.FindOrAdd(Vehicle);
    State = FDTTPoolTruckState();
    State.Phase = EDTTTruckPhase::ToDepot;
    if (DepotGuid.IsValid())
    {
        ClaimDepot(DepotGuid, Vehicle);
        State.DepotGuid = DepotGuid;
    }
    UE_LOG(LogDynamicTimeTableSubsystem, Display, TEXT("POOL_JOINED Truck='%s' Depot='%s' Reason=%s Pool=%d"),
        *DTSVehicleName(Vehicle), *DockDisplayName(DepotGuid), Reason, PoolTrucks.Num());
    return State;
}

FDTTPoolTruckState* UDynamicTimeTableSubsystem::EnsurePoolMembership(
    AFGWheeledVehicleIdentifier* Vehicle, const TArray<FGuid>& Route, const TCHAR* Reason)
{
    if (!IsValid(Vehicle)) return nullptr;
    if (FDTTPoolTruckState* Existing = PoolTrucks.Find(Vehicle)) return Existing;
    if (!RouteHasDepot(Route)) return nullptr;

    // The first depot in the route that nobody else claims is this truck's; otherwise it picks
    // the nearest free one at its next boundary.
    FGuid Depot;
    for (const FGuid& Node : Route)
    {
        if (!IsDepotGuid(Node)) continue;
        const TWeakObjectPtr<AFGWheeledVehicleIdentifier>* Claim = DepotClaims.Find(Node);
        if (!Claim || !Claim->IsValid() || Claim->Get() == Vehicle) { Depot = Node; break; }
    }
    return &JoinPool(Vehicle, Depot, Reason);
}

void UDynamicTimeTableSubsystem::LeavePool(AFGWheeledVehicleIdentifier* Vehicle, const TCHAR* Reason)
{
    FDTTPoolTruckState* State = PoolTrucks.Find(Vehicle);
    if (!State) return;
    if (State->Phase == EDTTTruckPhase::Parked) UnparkTruck(Vehicle, *State, EDTTTruckPhase::Unmanaged, Reason, true);
    ReleaseDepotClaim(Vehicle, Reason);
    PoolTrucks.Remove(Vehicle);
    UE_LOG(LogDynamicTimeTableSubsystem, Display, TEXT("POOL_LEFT Truck='%s' Reason=%s Pool=%d"),
        *DTSVehicleName(Vehicle), Reason, PoolTrucks.Num());
}

FGuid UDynamicTimeTableSubsystem::FindNearestDepot(AFGWheeledVehicleIdentifier* Vehicle, const FGuid& FromGuid) const
{
    if (!IsValid(Vehicle) || !FromGuid.IsValid() || DispatchDepotGuids.Num() == 0) return FGuid();
    AFGWheeledVehicle* WheeledVehicle = Vehicle->GetOwnerVehicle();
    if (!IsValid(WheeledVehicle)) return FGuid();

    AFGVehicleSubsystem* VehicleSubsystem = AFGVehicleSubsystem::Get(WheeledVehicle->GetWorld());
    const UFGVehiclePathPreset* Preset = WheeledVehicle->GetVehiclePathPreset();
    if (!IsValid(VehicleSubsystem) || !Preset) return FGuid();

    FGuid Nearest;
    float BestCost = TNumericLimits<float>::Max();
    for (const FGuid& Candidate : DispatchDepotGuids)
    {
        if (!Candidate.IsValid()) continue;
        const TWeakObjectPtr<AFGWheeledVehicleIdentifier>* Claim = DepotClaims.Find(Candidate);
        if (Claim && Claim->IsValid() && Claim->Get() != Vehicle) continue;
        if (!DepotFitsTruck(Candidate, Vehicle)) continue;
        // Standing at a free depot: that one, at no cost.
        const float Cost = Candidate == FromGuid ? 0.0f
            : EstimatePathFuelConsumptionMW(VehicleSubsystem, Preset, WheeledVehicle->GetClass(), FromGuid, Candidate);
        if (Cost < 0.0f) continue;
        if (Cost < BestCost) { BestCost = Cost; Nearest = Candidate; }
    }
    return Nearest;
}

bool UDynamicTimeTableSubsystem::DepotFitsTruck(const FGuid& DepotGuid, AFGWheeledVehicleIdentifier* Vehicle) const
{
    const AFGDockingStationIdentifier* Dock = FindDockByPathNode(DepotGuid);
    const AFGBuildableDockingStation* Station = IsValid(Dock) ? Dock->GetStation() : nullptr;
    if (!IsValid(Station) || !IsValid(Vehicle)) return true;
    return DTSIsFluidForm(Station->GetDockingStationResourceForm()) == IsFluidTruck(Vehicle);
}

void UDynamicTimeTableSubsystem::ClaimDepot(const FGuid& DepotGuid, AFGWheeledVehicleIdentifier* Vehicle)
{
    if (!DepotGuid.IsValid() || !IsValid(Vehicle)) return;
    // One depot per truck as well as one truck per depot.
    for (auto It = DepotClaims.CreateIterator(); It; ++It)
    {
        if (It.Key() != DepotGuid && It.Value().Get() == Vehicle) It.RemoveCurrent();
    }
    const TWeakObjectPtr<AFGWheeledVehicleIdentifier>* Existing = DepotClaims.Find(DepotGuid);
    if (Existing && Existing->Get() == Vehicle) return;
    DepotClaims.Add(DepotGuid, Vehicle);
    UE_LOG(LogDynamicTimeTableSubsystem, Display, TEXT("DEPOT_CLAIMED Depot='%s' Truck='%s' Claims=%d/%d"),
        *DockDisplayName(DepotGuid), *DTSVehicleName(Vehicle), DepotClaims.Num(), DispatchDepotGuids.Num());
}

void UDynamicTimeTableSubsystem::ReleaseDepotClaim(AFGWheeledVehicleIdentifier* Vehicle, const TCHAR* Reason)
{
    for (auto It = DepotClaims.CreateIterator(); It; ++It)
    {
        if (It.Value().Get() != Vehicle) continue;
        UE_LOG(LogDynamicTimeTableSubsystem, Verbose, TEXT("DEPOT_RELEASED Depot='%s' Truck='%s' Reason=%s"),
            *DockDisplayName(It.Key()), *DTSVehicleName(Vehicle), Reason);
        It.RemoveCurrent();
    }
}

void UDynamicTimeTableSubsystem::ParkTruck(
    AFGWheeledVehicleIdentifier* Vehicle, FDTTPoolTruckState& State, AFGBuildableDockingStation* Station)
{
    if (!IsValid(Vehicle) || !IsValid(Station)) return;
    const AFGDockingStationIdentifier* StationIdentifier = Station->GetStationIdentifier();
    const FGuid StationGuid = IsValid(StationIdentifier) ? StationIdentifier->GetPathNodeGUID() : FGuid();
    if (!StationGuid.IsValid()) return;

    // Possession wins over a claim: a truck that is heading here but has not arrived is sent to
    // pick another depot at its next boundary, rather than evicting this one on arrival.
    if (const TWeakObjectPtr<AFGWheeledVehicleIdentifier>* Claim = DepotClaims.Find(StationGuid))
    {
        AFGWheeledVehicleIdentifier* Other = Claim->Get();
        if (IsValid(Other) && Other != Vehicle)
        {
            if (FDTTPoolTruckState* OtherState = PoolTrucks.Find(Other))
            {
                OtherState->DepotGuid.Invalidate();
                PendingRouteReevaluations.Add(Other);
                SchedulePendingRouteReevaluation();
            }
            UE_LOG(LogDynamicTimeTableSubsystem, Display, TEXT("DEPOT_CLAIM_TAKEN Depot='%s' From='%s' By='%s'"),
                *DockDisplayName(StationGuid), *DTSVehicleName(Other), *DTSVehicleName(Vehicle));
        }
    }
    if (!DepotFitsTruck(StationGuid, Vehicle))
    {
        UE_LOG(LogDynamicTimeTableSubsystem, Warning, TEXT("DEPOT_FORM_MISMATCH Truck='%s' Depot='%s' TruckForm=%s Action=parked-no-unload"),
            *DTSVehicleName(Vehicle), *DockDisplayName(StationGuid), IsFluidTruck(Vehicle) ? TEXT("fluid") : TEXT("solid"));
    }
    ClaimDepot(StationGuid, Vehicle);
    State.DepotGuid = StationGuid;
    State.Phase = EDTTTruckPhase::Parked;
    State.bParked = true;
    State.PlannedItems.Reset();
    State.bCargoWaitLogged = false;
    State.ChainFromGuid.Invalidate();
    // The lap is over: whatever it still claimed is free for the next one.
    ReleaseRequesterClaimsForVehicle(Vehicle, TEXT("parked"));
    ReleaseTruckBooking(Vehicle, TEXT("parked"));
    FMyDTTHooks::SetHeld(Station, true, TEXT("parked"));
    UE_LOG(LogDynamicTimeTableSubsystem, Display, TEXT("PARKED Truck='%s' Depot='%s' HoldInstalled=%s"),
        *DTSVehicleName(Vehicle), *DockDisplayName(StationGuid), FMyDTTHooks::IsUndockHoldInstalled() ? TEXT("true") : TEXT("false"));
    UpdatePoolVehicleName(Vehicle, State);
    RunPoolDispatch(TEXT("truck parked"));
}

bool UDynamicTimeTableSubsystem::ReleaseParkedTruck(
    AFGWheeledVehicleIdentifier* Vehicle, FDTTPoolTruckState& State, const TArray<FGuid>& NewRoute,
    EDTTTruckPhase NewPhase, const TCHAR* Reason)
{
    if (!IsValid(Vehicle)) return false;
    const AFGDockingStationIdentifier* Dock = FindDockByPathNode(State.DepotGuid);
    AFGBuildableDockingStation* Station = IsValid(Dock) ? Dock->GetStation() : nullptr;

    // Route first, hold second, undock last: the truck must already know where to go when the
    // autopilot resumes, and the game re-saves the route at the end of a docking (see
    // ROUTE_REVERTED_BY_GAME), which ReplaceRoute records for HandleVehicleRouteChanged.
    if (!ReplaceRoute(Vehicle, NewRoute))
    {
        UE_LOG(LogDynamicTimeTableSubsystem, Warning, TEXT("RELEASE_FAILED Truck='%s' Depot='%s' Stops=%d Reason=route-not-written"),
            *DTSVehicleName(Vehicle), *DockDisplayName(State.DepotGuid), NewRoute.Num());
        return false;
    }
    State.Phase = NewPhase;
    State.bParked = false;
    if (IsValid(Station))
    {
        FMyDTTHooks::SetHeld(Station, false, Reason);
        // Mid-cycle the game's own deferred undock is still coming; forcing a second one here
        // would undock twice.
        if (!Station->IsLoadUnloading() && Station->GetDockedActor() == Vehicle->GetOwnerVehicle()) Station->ForceUndockActor();
    }
    UE_LOG(LogDynamicTimeTableSubsystem, Display, TEXT("RELEASED Truck='%s' Depot='%s' Phase=%s Stops=%d Reason=%s"),
        *DTSVehicleName(Vehicle), *DockDisplayName(State.DepotGuid), *UEnum::GetDisplayValueAsText(NewPhase).ToString(), NewRoute.Num(), Reason);
    UpdatePoolVehicleName(Vehicle, State);
    return true;
}

void UDynamicTimeTableSubsystem::UnparkTruck(
    AFGWheeledVehicleIdentifier* Vehicle, FDTTPoolTruckState& State, EDTTTruckPhase NewPhase, const TCHAR* Reason, bool bUndock)
{
    const AFGDockingStationIdentifier* Dock = FindDockByPathNode(State.DepotGuid);
    AFGBuildableDockingStation* Station = IsValid(Dock) ? Dock->GetStation() : nullptr;
    State.Phase = NewPhase;
    State.bParked = false;
    if (IsValid(Station) && FMyDTTHooks::IsHeld(Station))
    {
        FMyDTTHooks::SetHeld(Station, false, Reason);
        if (bUndock && IsValid(Vehicle) && Station->GetDockedActor() == Vehicle->GetOwnerVehicle() && !Station->IsLoadUnloading())
        {
            Station->ForceUndockActor();
        }
    }
    UE_LOG(LogDynamicTimeTableSubsystem, Display, TEXT("UNPARKED Truck='%s' Depot='%s' Phase=%s Undock=%s Reason=%s"),
        *DTSVehicleName(Vehicle), *DockDisplayName(State.DepotGuid), *UEnum::GetDisplayValueAsText(NewPhase).ToString(),
        bUndock ? TEXT("true") : TEXT("false"), Reason);
    if (IsValid(Vehicle)) UpdatePoolVehicleName(Vehicle, State);
}

void UDynamicTimeTableSubsystem::ProcessPoolTruckTarget(
    AFGWheeledVehicleIdentifier* Vehicle, FDTTPoolTruckState& State, const TCHAR* Trigger)
{
    if (!IsValid(Vehicle)) return;
    switch (State.Phase)
    {
    case EDTTTruckPhase::ToDepot:
        break;
    case EDTTTruckPhase::Assigned:
    {
        // Same two boundaries as a legacy lap: past the producer the booking goes, and a truck
        // that is empty before its last requester turns for home early - here home is the depot
        // at the tail, not the producer. Index 0 is the target wrapping to the producer, which
        // happens on arrival at the last stop, the depot: HandleTruckDocked parks it there.
        const TArray<FGuid> Route = Vehicle->GetVehicleRoute();
        const int32 Index = Vehicle->GetCurrentTargetWaypointIndex();
        // The producer is not necessarily Route[0]: the lap may start with the depot it was
        // released from (AssignPoolTruck), so "past the producer" is measured from its position.
        const int32 ProducerIndex = FMath::Max(0, Route.IndexOfByKey(State.ProducerGuid));
        if (Index > ProducerIndex)
        {
            ReleaseTruckBooking(Vehicle, TEXT("left producer"));
            if (CutLapIfCargoEmpty(Vehicle, State.ResourceId, Route, Index, Trigger, State.DepotGuid))
            {
                // The lap now ends at the requester the truck has just left. Trimming the plan to
                // the stops that survived makes that requester the last one, so the undock that
                // brought us here chains the next lap from there (HandleTruckUndocked) when a
                // request is open - and the truck drives on to the depot when none is. With
                // exact quantities off this is the normal end of a lap: a requester takes
                // everything that fits, and the truck is usually empty after the first or second.
                const TArray<FGuid>& NewRoute = Vehicle->GetVehicleRoute();
                const int32 Before = State.Requesters.Num();
                State.Requesters.RemoveAll([&](const FGuid& Guid) { return !NewRoute.Contains(Guid); });
                for (auto It = State.PlannedItems.CreateIterator(); It; ++It)
                {
                    if (!NewRoute.Contains(It.Key())) It.RemoveCurrent();
                }
                UE_LOG(LogDynamicTimeTableSubsystem, Display, TEXT("POOL_LAP_TRIMMED Truck='%s' Resource='%s' Requesters=%d->%d Trigger=%s"),
                    *DTSVehicleName(Vehicle), *State.ResourceId, Before, State.Requesters.Num(), Trigger);
            }
        }
        // Every leg of the lap, the one to the depot included, on the route as the cut left it.
        {
            FGuid Emergency;
            if (EnsureFuelForNextLeg(Vehicle, false, Emergency, Trigger)) State.EmergencyFuelGuid = Emergency;
        }
        UpdatePoolVehicleName(Vehicle, State);
        return;
    }
    default:
        // Parked: nothing to do until a request comes. Manual: the player's route runs as saved.
        return;
    }

    const TArray<FGuid>& Route = Vehicle->GetVehicleRoute();
    const int32 Index = Vehicle->GetCurrentTargetWaypointIndex();
    const FGuid From = Route.IsValidIndex(Index) ? Route[Index] : (Route.Num() > 0 ? Route[0] : FGuid());

    // The claimed depot, if it is still a depot and still this truck's; otherwise the nearest one
    // that nobody claims, measured from where the truck is heading now.
    FGuid Depot = State.DepotGuid;
    if (Depot.IsValid())
    {
        const TWeakObjectPtr<AFGWheeledVehicleIdentifier>* Claim = DepotClaims.Find(Depot);
        if (!IsDepotGuid(Depot) || (Claim && Claim->IsValid() && Claim->Get() != Vehicle)) Depot.Invalidate();
        else if (!DepotFitsTruck(Depot, Vehicle))
        {
            // Claimed before the form rule existed, or the depot's station was rebuilt as the other kind.
            UE_LOG(LogDynamicTimeTableSubsystem, Warning, TEXT("DEPOT_FORM_MISMATCH Truck='%s' Depot='%s' TruckForm=%s Action=released"),
                *DTSVehicleName(Vehicle), *DockDisplayName(Depot), IsFluidTruck(Vehicle) ? TEXT("fluid") : TEXT("solid"));
            ReleaseDepotClaim(Vehicle, TEXT("depot form"));
            Depot.Invalidate();
        }
    }
    if (!Depot.IsValid())
    {
        Depot = FindNearestDepot(Vehicle, From);
        if (!Depot.IsValid())
        {
            UE_LOG(LogDynamicTimeTableSubsystem, Display, TEXT("DEPOT_NONE_FREE Truck='%s' From='%s' TruckForm=%s Depots=%d Claims=%d Trigger=%s"),
                *DTSVehicleName(Vehicle), *DockDisplayName(From), IsFluidTruck(Vehicle) ? TEXT("fluid") : TEXT("solid"),
                DispatchDepotGuids.Num(), DepotClaims.Num(), Trigger);
            State.DepotGuid.Invalidate();
            return;
        }
        ClaimDepot(Depot, Vehicle);
        State.DepotGuid = Depot;
    }

    // A pump the leg guard put in front of the depot is still ahead: keep driving to it.
    if (State.EmergencyFuelGuid.IsValid() && Depot == State.DepotGuid &&
        Route.IsValidIndex(Index) && Route[Index] == State.EmergencyFuelGuid)
    {
        UpdatePoolVehicleName(Vehicle, State);
        return;
    }
    State.EmergencyFuelGuid.Invalidate();

    // Twice the same node is a legal route (TooFewStations is Num() < 2) and the only one that
    // keeps the truck circling back to the depot without visiting anything else.
    TArray<FGuid> NewRoute{ Depot, Depot };
    // The way home is a leg like any other: a tank that cannot cover it twice over goes to the
    // nearest reachable pump first. The route is shaped to the current target index the way
    // AssignPoolTruck shapes a lap - copies of the depot in front, the pump exactly on the target.
    float Need = 0.0f, Energy = 0.0f;
    if (From != Depot && !DispatchFuelStopGuids.Contains(From) && !TankCoversLeg(Vehicle, From, Depot, 0.0f, Need, Energy))
    {
        const FGuid Fuel = ChooseFuelStopFromGroup(Vehicle, FindNearestFuelStop(Vehicle->GetOwnerVehicle(), From));
        if (Fuel.IsValid() && Fuel != Depot)
        {
            const int32 TargetIndex = FMath::Clamp(Index, 0, 3);
            NewRoute.Reset();
            for (int32 Copy = 0; Copy < TargetIndex; ++Copy) NewRoute.Add(Depot);
            NewRoute.Add(Fuel);
            NewRoute.Add(Depot);
            State.EmergencyFuelGuid = Fuel;
            UE_LOG(LogDynamicTimeTableSubsystem, Display,
                TEXT("FUEL_LEG_GUARD Truck='%s' From='%s' To='%s' LegCost=%.3f Energy=%.3f Fuel='%s' At=%d Reachable=- Trigger=%s"),
                *DTSVehicleName(Vehicle), *DockDisplayName(From), *DockDisplayName(Depot), Need, Energy,
                *DockDisplayName(Fuel), TargetIndex, Trigger);
        }
        else
        {
            UE_LOG(LogDynamicTimeTableSubsystem, Warning,
                TEXT("FUEL_LEG_GUARD_NO_STOP Truck='%s' From='%s' To='%s' LegCost=%.3f Energy=%.3f Trigger=%s"),
                *DTSVehicleName(Vehicle), *DockDisplayName(From), *DockDisplayName(Depot), Need, Energy, Trigger);
        }
    }
    if (Route != NewRoute)
    {
        const bool bWritten = ReplaceRoute(Vehicle, NewRoute);
        if (!bWritten) State.EmergencyFuelGuid.Invalidate();
        UE_LOG(LogDynamicTimeTableSubsystem, Display, TEXT("POOL_TO_DEPOT Truck='%s' Depot='%s' Written=%s Trigger=%s"),
            *DTSVehicleName(Vehicle), *DockDisplayName(Depot), bWritten ? TEXT("true") : TEXT("false"), Trigger);
    }
    UpdatePoolVehicleName(Vehicle, State);
}

FGuid UDynamicTimeTableSubsystem::ChooseProducerForResource(
    AFGWheeledVehicleIdentifier* Vehicle, const FString& ResourceId,
    TSubclassOf<UFGItemDescriptor> ItemClass, const FGuid& FromGuid, int32& OutStockStacks) const
{
    OutStockStacks = 0;
    if (!ItemClass) return FGuid();
    AFGWheeledVehicle* WheeledVehicle = IsValid(Vehicle) ? Vehicle->GetOwnerVehicle() : nullptr;
    AFGVehicleSubsystem* VehicleSubsystem = IsValid(WheeledVehicle) ? AFGVehicleSubsystem::Get(WheeledVehicle->GetWorld()) : nullptr;
    const UFGVehiclePathPreset* Preset = IsValid(WheeledVehicle) ? WheeledVehicle->GetVehiclePathPreset() : nullptr;
    const bool bCanCost = IsValid(VehicleSubsystem) && Preset && FromGuid.IsValid();

    FGuid Best;
    int32 BestStock = 0;
    float BestCost = TNumericLimits<float>::Max();
    for (const FDTTDispatchStation& Station : DispatchStations)
    {
        if (GetEffectiveDispatchRole(Station) != EDTTDispatchRole::Producer ||
            !Station.ResourceId.Equals(ResourceId, ESearchCase::IgnoreCase) ||
            !DoesDispatchStationMatchMode(Station) || !Station.DockingPathNodeGuid.IsValid())
        {
            continue;
        }
        const int32 Stock = ReadDockStockInStacks(Station.DockingStationIdentifier.Get(), ItemClass);
        if (Stock <= 0) continue;
        float Cost = 0.0f;
        if (bCanCost)
        {
            Cost = Station.DockingPathNodeGuid == FromGuid ? 0.0f
                : EstimatePathFuelConsumptionMW(VehicleSubsystem, Preset, WheeledVehicle->GetClass(), FromGuid, Station.DockingPathNodeGuid);
            // Not on this truck's path network: not a place it can load at.
            if (Cost < 0.0f) continue;
        }
        UE_LOG(LogDynamicTimeTableSubsystem, VeryVerbose, TEXT("POOL_PRODUCER_CANDIDATE Resource='%s' Producer='%s' Stock=%d Cost=%.3f"),
            *ResourceId, *Station.DisplayName, Stock, Cost);
        if (Stock > BestStock || (Stock == BestStock && Cost < BestCost))
        {
            Best = Station.DockingPathNodeGuid;
            BestStock = Stock;
            BestCost = Cost;
        }
    }
    OutStockStacks = BestStock;
    return Best;
}

bool UDynamicTimeTableSubsystem::AssignPoolTruck(
    AFGWheeledVehicleIdentifier* Vehicle, FDTTPoolTruckState& State, const FString& ResourceId,
    TSubclassOf<UFGItemDescriptor> ItemClass, const FGuid& ProducerGuid, const TCHAR* Trigger)
{
    if (!IsValid(Vehicle)) return false;
    // A truck that has just left the last requester of its lap (ChainFromGuid) starts the next lap
    // from there; the depot it was driving back to only shapes the route (see below).
    const bool bChained = State.Phase == EDTTTruckPhase::Assigned && State.ChainFromGuid.IsValid();
    if (State.Phase != EDTTTruckPhase::Parked && !bChained) return false;
    const FGuid StartDepot = State.DepotGuid;
    const FGuid From = bChained ? State.ChainFromGuid : StartDepot;
    // Claims first, like a legacy lap: two trucks must not plan the same requester in one pass.
    TMap<FGuid, int32> Planned;
    int32 NeededStacks = 0;
    // At a depot in UNLOAD mode the cargo goes into the depot at the end of this very docking
    // cycle (measured: a lap planned as "cargo covers" left with an empty truck), so the lap is
    // planned as if the truck were empty.
    const bool bCargoDrained = IsCargoDrainedAtDepot(State);
    const TArray<FGuid> Requesters = SelectRequestersForResource(ResourceId, Vehicle, ProducerGuid, &Planned, &NeededStacks, bCargoDrained);
    if (Requesters.Num() == 0)
    {
        ReleaseTruckBooking(Vehicle, TEXT("no requesters"));
        UE_LOG(LogDynamicTimeTableSubsystem, Verbose, TEXT("POOL_SKIP Truck='%s' Resource='%s' Reason=no-requesters Trigger=%s"),
            *DTSVehicleName(Vehicle), *ResourceId, Trigger);
        return false;
    }
    // What is already on board may cover the whole plan; then the producer is a detour. No
    // producer at all (none has stock) means the cargo is all there is.
    const int32 OnBoardStacks = bCargoDrained ? 0 : OnBoardStacksOf(Vehicle, ItemClass);
    FGuid Producer = ProducerGuid;
    const TCHAR* ProducerNote = TEXT("stock");
    if (!Producer.IsValid())
    {
        ProducerNote = TEXT("cargo only");
    }
    else if (Settings.bSkipProducerWhenCargoCovers && OnBoardStacks > 0 && OnBoardStacks >= NeededStacks)
    {
        ReleaseTruckBooking(Vehicle, TEXT("cargo covers the lap"));
        Producer.Invalidate();
        ProducerNote = TEXT("cargo covers");
    }

    // Producer, the requesters, and the nearest free depot from the last of them: the truck ends
    // the lap where the next one is likely to start, not necessarily where it slept.
    //
    // The autopilot keeps its target index across a route write, and at the depot that index is
    // whatever the [Depot, Depot] loop left it at - 1 as often as 0 (measured: the first pool lap
    // drove straight to the requester with an empty truck). SetCurrentTargetWaypoint is a status
    // setter, not a command (1.0.7), so the route is shaped to the index instead: as many copies
    // of the depot in front as the index says, and the producer lands exactly on the target.
    // A chained lap starts with the index pointing at the old lap's return depot, which can sit
    // anywhere in a route of up to eight stops; the depot copies in front are never driven to
    // unless the loop wraps, and then the truck only parks.
    const int32 TargetIndex = FMath::Clamp(Vehicle->GetCurrentTargetWaypointIndex(), 0, bChained ? 8 : 3);
    TArray<FGuid> NewRoute;
    for (int32 Copy = 0; Copy < TargetIndex && StartDepot.IsValid(); ++Copy) NewRoute.Add(StartDepot);
    // Without a producer the first requester lands on the target instead.
    const int32 FirstStopIndex = NewRoute.Num();
    const int32 ProducerIndex = Producer.IsValid() ? FirstStopIndex : INDEX_NONE;
    if (Producer.IsValid()) NewRoute.Add(Producer);
    NewRoute.Append(Requesters);
    FGuid ReturnDepot = FindNearestDepot(Vehicle, Requesters.Last());
    if (!ReturnDepot.IsValid()) ReturnDepot = StartDepot;
    NewRoute.Add(ReturnDepot);

    // Fuel: the estimate treats the lap as a closed loop, so the Depot -> Producer leg counts
    // once more than it is driven. Conservative, and the pump goes before the depot.
    FGuid FuelGuid;
    int32 FuelIndex = INDEX_NONE;
    if (IsFuelCriticallyLow(Vehicle, NewRoute))
    {
        // The nearest (F) names the group, the least-loaded member of it gets the visit.
        FuelGuid = ChooseFuelStopFromGroup(Vehicle, FindNearestFuelStop(Vehicle->GetOwnerVehicle(), NewRoute[FirstStopIndex]));
        if (FuelGuid.IsValid() && !NewRoute.Contains(FuelGuid))
        {
            // After the first stop, before the depot.
            FuelIndex = FMath::Clamp(ChooseFuelInsertIndex(Vehicle, NewRoute, FuelGuid, FirstStopIndex + 1), FirstStopIndex + 1, NewRoute.Num() - 1);
            NewRoute.Insert(FuelGuid, FuelIndex);
        }
        else if (!FuelGuid.IsValid())
        {
            UE_LOG(LogDynamicTimeTableSubsystem, Warning, TEXT("DISPATCH_FUEL_NO_STOP_FOUND Truck='%s' Resource='%s' Trigger=%s"),
                *DTSVehicleName(Vehicle), *ResourceId, Trigger);
        }
    }

    // The name and the transfer plan read these, so they go in before the truck moves; undone if
    // the route cannot be written.
    State.ResourceId = ResourceId;
    State.ProducerGuid = Producer;
    State.Requesters = Requesters;
    State.FuelGuid = FuelGuid;
    State.EmergencyFuelGuid.Invalidate();
    State.PlannedItems = MoveTemp(Planned);
    const UWorld* World = GetWorld();
    State.AssignedAtSeconds = IsValid(World) ? World->GetTimeSeconds() : 0.0;

    // Chained: no hold to lift and nothing to undock, the truck is already on the road.
    const bool bWritten = bChained
        ? ReplaceRoute(Vehicle, NewRoute)
        : ReleaseParkedTruck(Vehicle, State, NewRoute, EDTTTruckPhase::Assigned, TEXT("assigned"));
    if (!bWritten)
    {
        ReleaseRequesterClaimsForVehicle(Vehicle, TEXT("assignment failed"));
        ReleaseTruckBooking(Vehicle, TEXT("assignment failed"));
        State.ResourceId.Reset();
        State.ProducerGuid.Invalidate();
        State.Requesters.Reset();
        State.FuelGuid.Invalidate();
        State.PlannedItems.Reset();
        UE_LOG(LogDynamicTimeTableSubsystem, Warning, TEXT("POOL_ASSIGN_FAILED Truck='%s' Resource='%s' Trigger=%s"),
            *DTSVehicleName(Vehicle), *ResourceId, Trigger);
        return false;
    }
    if (ReturnDepot != StartDepot)
    {
        ClaimDepot(ReturnDepot, Vehicle);
        State.DepotGuid = ReturnDepot;
    }
    State.ChainFromGuid.Invalidate();
    int32 PlannedItems = 0;
    for (const auto& Pair : State.PlannedItems) PlannedItems += Pair.Value;
    UE_LOG(LogDynamicTimeTableSubsystem, Display,
        TEXT("POOL_ASSIGNED Truck='%s' Resource='%s' Producer='%s' ProducerIndex=%d ProducerNote=%s OnBoard=%d Needed=%d Requesters=%d PlannedItems=%d Fuel=%s FuelIndex=%d From='%s' Depot='%s' Stops=%d Trigger=%s"),
        *DTSVehicleName(Vehicle), *ResourceId, Producer.IsValid() ? *DockDisplayName(Producer) : TEXT("-"), ProducerIndex, ProducerNote,
        OnBoardStacks, NeededStacks, Requesters.Num(), PlannedItems,
        FuelGuid.IsValid() ? *DockDisplayName(FuelGuid) : TEXT("-"), FuelIndex,
        *DockDisplayName(From), *DockDisplayName(ReturnDepot), NewRoute.Num(), Trigger);
    // The first leg of the lap is driven from where the truck stands - the depot, or the requester
    // it chained from - and no target change announces it, so the leg guard runs here.
    {
        FGuid Emergency;
        if (EnsureFuelForNextLeg(Vehicle, false, Emergency, Trigger, From)) State.EmergencyFuelGuid = Emergency;
    }
    UpdatePoolVehicleName(Vehicle, State);
    return true;
}

void UDynamicTimeTableSubsystem::RunPoolDispatch(const TCHAR* Trigger)
{
    const UWorld* World = GetWorld();
    LastPoolDispatchSeconds = IsValid(World) ? World->GetTimeSeconds() : 0.0;

    // Pointers into PoolTrucks: nothing below adds or removes an entry.
    TArray<TPair<AFGWheeledVehicleIdentifier*, FDTTPoolTruckState*>> Idle;
    for (auto& Pair : PoolTrucks)
    {
        AFGWheeledVehicleIdentifier* Vehicle = Pair.Key.Get();
        if (!IsValid(Vehicle)) continue;
        // Parked, or on the way back from the last requester of a lap: that truck is as free as a
        // parked one and closer to the next request than its depot is.
        const bool bIdle = Pair.Value.Phase == EDTTTruckPhase::Parked ||
            (Pair.Value.Phase == EDTTTruckPhase::Assigned && Pair.Value.ChainFromGuid.IsValid());
        if (!bIdle) continue;
        if (!Vehicle->IsAutopilotEnabled() || !IsValid(Vehicle->GetOwnerVehicle())) continue;
        Idle.Emplace(Vehicle, &Pair.Value);
    }
    if (Idle.Num() == 0)
    {
        UE_LOG(LogDynamicTimeTableSubsystem, Verbose, TEXT("POOL_DISPATCH Trigger=%s Parked=0 Pool=%d Depots=%d"),
            Trigger, PoolTrucks.Num(), DispatchDepotGuids.Num());
        return;
    }
    PruneStaleRequesterClaims();
    RemoveInvalidTruckBookings();

    // Every resource with at least one open, unclaimed requester; the most starved resource first.
    struct FDemand { FString ResourceId; double Starved = 1.0; int32 Open = 0; FGuid TopRequester; };
    TArray<FDemand> Demands;
    TSet<FString> Seen;
    for (const FDTTDispatchStation& Station : DispatchStations)
    {
        if (GetEffectiveDispatchRole(Station) != EDTTDispatchRole::Requester || !DoesDispatchStationMatchMode(Station)) continue;
        const FString Key = Station.ResourceId.ToLower();
        if (Seen.Contains(Key)) continue;
        Seen.Add(Key);
        const TSubclassOf<UFGItemDescriptor> ItemClass = ResolveItemClassForResource(Station.ResourceId);
        if (!ItemClass)
        {
            UE_LOG(LogDynamicTimeTableSubsystem, Verbose, TEXT("POOL_SKIP Resource='%s' Reason=item-class-not-found"), *Station.ResourceId);
            continue;
        }
        TArray<FDTTOpenRequester> Open;
        CollectOpenRequesters(Station.ResourceId, ItemClass, nullptr, Open);
        if (Open.Num() == 0) continue;
        Demands.Add(FDemand{ Station.ResourceId, Open[0].FillRatio, Open.Num(), Open[0].Station->DockingPathNodeGuid });
    }
    Demands.Sort([](const FDemand& A, const FDemand& B)
    {
        if (!FMath::IsNearlyEqual(A.Starved, B.Starved)) return A.Starved < B.Starved;
        return A.ResourceId < B.ResourceId;
    });

    const bool bTimerPass = FCString::Strcmp(Trigger, TEXT("timer")) == 0;
    int32 Assigned = 0;
    for (const FDemand& Demand : Demands)
    {
        if (Idle.Num() == 0) break;
        const TSubclassOf<UFGItemDescriptor> ItemClass = ResolveItemClassForResource(Demand.ResourceId);
        int32 ProducerStock = 0;
        FGuid Producer = ChooseProducerForResource(Idle[0].Key, Demand.ResourceId, ItemClass, Idle[0].Value->DepotGuid, ProducerStock);
        // No producer with stock is not the end: a parked truck still carrying the resource can
        // deliver from its cargo. The lap then goes straight to the requesters.
        const bool bFromCargo = !Producer.IsValid();
        const FGuid Target = bFromCargo ? Demand.TopRequester : Producer;
        const AFGDockingStationIdentifier* TargetDock = FindDockByPathNode(Target);
        const AFGBuildableDockingStation* TargetStation = IsValid(TargetDock) ? TargetDock->GetStation() : nullptr;
        if (!IsValid(TargetStation)) continue;
        const EResourceForm ProducerForm = TargetStation->GetDockingStationResourceForm();

        // The cheapest truck that can carry the form and is not carrying something else.
        int32 BestIdle = INDEX_NONE;
        float BestCost = TNumericLimits<float>::Max();
        for (int32 IdleIndex = 0; IdleIndex < Idle.Num(); ++IdleIndex)
        {
            AFGWheeledVehicleIdentifier* Vehicle = Idle[IdleIndex].Key;
            const FDTTPoolTruckState& State = *Idle[IdleIndex].Value;
            AFGWheeledVehicle* WheeledVehicle = Vehicle->GetOwnerVehicle();
            const EResourceForm TruckForm = IFGDockableInterface::Execute_GetDockInventoryResourceForm(WheeledVehicle);
            if (DTSIsFluidForm(TruckForm) != DTSIsFluidForm(ProducerForm))
            {
                UE_LOG(LogDynamicTimeTableSubsystem, Verbose, TEXT("POOL_SKIP Truck='%s' Resource='%s' Reason=form Trigger=%s"),
                    *DTSVehicleName(Vehicle), *Demand.ResourceId, Trigger);
                continue;
            }
            const bool bCargoDrained = IsCargoDrainedAtDepot(State);
            bool bForeignCargo = false;
            if (UFGInventoryComponent* Storage = bCargoDrained ? nullptr : WheeledVehicle->GetStorageInventory())
            {
                TArray<FInventoryStack> Stacks;
                Storage->GetInventoryStacks(Stacks);
                for (const FInventoryStack& Stack : Stacks)
                {
                    if (Stack.HasItems() && Stack.Item.GetItemClass() != ItemClass) { bForeignCargo = true; break; }
                }
            }
            if (bForeignCargo)
            {
                UE_LOG(LogDynamicTimeTableSubsystem, Verbose, TEXT("POOL_SKIP Truck='%s' Resource='%s' Reason=cargo Trigger=%s"),
                    *DTSVehicleName(Vehicle), *Demand.ResourceId, Trigger);
                continue;
            }
            if (bFromCargo && (bCargoDrained || OnBoardStacksOf(Vehicle, ItemClass) <= 0))
            {
                UE_LOG(LogDynamicTimeTableSubsystem, Verbose, TEXT("POOL_SKIP Truck='%s' Resource='%s' Reason=no-cargo Trigger=%s"),
                    *DTSVehicleName(Vehicle), *Demand.ResourceId, Trigger);
                continue;
            }
            const FGuid From = State.ChainFromGuid.IsValid() ? State.ChainFromGuid
                : State.DepotGuid.IsValid() ? State.DepotGuid
                : (Vehicle->GetVehicleRoute().Num() > 0 ? Vehicle->GetVehicleRoute()[0] : FGuid());
            AFGVehicleSubsystem* VehicleSubsystem = AFGVehicleSubsystem::Get(WheeledVehicle->GetWorld());
            const UFGVehiclePathPreset* Preset = WheeledVehicle->GetVehiclePathPreset();
            float Cost = 0.0f;
            if (IsValid(VehicleSubsystem) && Preset && From.IsValid() && From != Target)
            {
                Cost = EstimatePathFuelConsumptionMW(VehicleSubsystem, Preset, WheeledVehicle->GetClass(), From, Target);
                if (Cost < 0.0f)
                {
                    UE_LOG(LogDynamicTimeTableSubsystem, Verbose, TEXT("POOL_SKIP Truck='%s' Resource='%s' Reason=unreachable Trigger=%s"),
                        *DTSVehicleName(Vehicle), *Demand.ResourceId, Trigger);
                    continue;
                }
            }
            if (Cost < BestCost) { BestCost = Cost; BestIdle = IdleIndex; }
        }
        if (BestIdle == INDEX_NONE)
        {
            // The timer pass repeats every 5 s while a demand stays unserved; only a pass with a
            // reason to expect a change (a truck just parked) says so at Display.
            const TCHAR* Why = bFromCargo ? TEXT("no-producer-with-stock") : TEXT("no-suitable-truck");
            if (bTimerPass)
            {
                UE_LOG(LogDynamicTimeTableSubsystem, Verbose, TEXT("POOL_SKIP Resource='%s' Open=%d Reason=%s Idle=%d Trigger=%s"), *Demand.ResourceId, Demand.Open, Why, Idle.Num(), Trigger);
            }
            else
            {
                UE_LOG(LogDynamicTimeTableSubsystem, Display, TEXT("POOL_SKIP Resource='%s' Open=%d Reason=%s Idle=%d Trigger=%s"), *Demand.ResourceId, Demand.Open, Why, Idle.Num(), Trigger);
            }
            continue;
        }

        AFGWheeledVehicleIdentifier* Chosen = Idle[BestIdle].Key;
        FDTTPoolTruckState* ChosenState = Idle[BestIdle].Value;
        // With the producer in a [group], the group rule picks which member this lap loads at
        // and books it, exactly as for a legacy lap.
        if (Producer.IsValid())
        {
            const FGuid Balanced = ChooseProducerFromGroup(Chosen, Demand.ResourceId, Producer);
            if (Balanced.IsValid()) Producer = Balanced;
        }
        UE_LOG(LogDynamicTimeTableSubsystem, Verbose, TEXT("POOL_MATCH Truck='%s' Resource='%s' Producer='%s' Stock=%d Cost=%.3f Open=%d"),
            *DTSVehicleName(Chosen), *Demand.ResourceId, *DockDisplayName(Producer), ProducerStock, BestCost, Demand.Open);
        if (AssignPoolTruck(Chosen, *ChosenState, Demand.ResourceId, ItemClass, Producer, Trigger))
        {
            ++Assigned;
            Idle.RemoveAt(BestIdle);
        }
    }
    // Once per parking: a truck still parked with cargo after every demand was tried sits with
    // something nobody asked for. It waits for a request of that very item, or for a depot in
    // UNLOAD mode to take it. Logged here, not per demand, so a truck the same round dispatches
    // for its own cargo is not reported as waiting.
    for (const auto& Entry : Idle)
    {
        FDTTPoolTruckState& Mutable = *Entry.Value;
        if (Mutable.Phase != EDTTTruckPhase::Parked || Mutable.bCargoWaitLogged || IsCargoDrainedAtDepot(Mutable)) continue;
        TSubclassOf<UFGItemDescriptor> Single;
        const FString Cargo = DescribeCargo(Entry.Key, &Single);
        if (Cargo.IsEmpty()) continue;
        Mutable.bCargoWaitLogged = true;
        UE_LOG(LogDynamicTimeTableSubsystem, Display, TEXT("POOL_CARGO_WAIT Truck='%s' Cargo='%s' Depot='%s' Reason=%s"),
            *DTSVehicleName(Entry.Key), *Cargo, *DockDisplayName(Mutable.DepotGuid),
            Single ? TEXT("no open request for this item") : TEXT("mixed cargo"));
    }
    if (Assigned > 0)
    {
        UE_LOG(LogDynamicTimeTableSubsystem, Display, TEXT("POOL_DISPATCH Trigger=%s Parked=%d Demands=%d Assigned=%d Pool=%d Depots=%d"),
            Trigger, Idle.Num() + Assigned, Demands.Num(), Assigned, PoolTrucks.Num(), DispatchDepotGuids.Num());
    }
    else
    {
        UE_LOG(LogDynamicTimeTableSubsystem, Verbose, TEXT("POOL_DISPATCH Trigger=%s Parked=%d Demands=%d Assigned=0 Pool=%d Depots=%d"),
            Trigger, Idle.Num(), Demands.Num(), PoolTrucks.Num(), DispatchDepotGuids.Num());
    }
}

void UDynamicTimeTableSubsystem::RestoreParkedTrucks()
{
    if (bParkedTrucksRestored || !IsServer()) return;
    bParkedTrucksRestored = true;

    int32 Restored = 0, Stale = 0;
    for (auto& Pair : PoolTrucks)
    {
        AFGWheeledVehicleIdentifier* Vehicle = Pair.Key.Get();
        FDTTPoolTruckState& State = Pair.Value;
        if (!IsValid(Vehicle)) continue;

        if (State.DepotGuid.IsValid() && IsDepotGuid(State.DepotGuid))
        {
            ClaimDepot(State.DepotGuid, Vehicle);
            if (!DepotFitsTruck(State.DepotGuid, Vehicle))
            {
                UE_LOG(LogDynamicTimeTableSubsystem, Warning, TEXT("DEPOT_FORM_MISMATCH Truck='%s' Depot='%s' TruckForm=%s Action=%s"),
                    *DTSVehicleName(Vehicle), *DockDisplayName(State.DepotGuid), IsFluidTruck(Vehicle) ? TEXT("fluid") : TEXT("solid"),
                    State.Phase == EDTTTruckPhase::Parked ? TEXT("kept-until-next-lap") : TEXT("re-picked-on-next-target"));
            }
        }
        else State.DepotGuid.Invalidate();
        if (State.Phase != EDTTTruckPhase::Parked) { State.bParked = false; continue; }

        // The game saved the docked actor and the docking flag; only the hold has to be put back.
        // Anything else - the truck was moved, the station rebuilt - is a stale record.
        const AFGDockingStationIdentifier* Dock = FindDockByPathNode(State.DepotGuid);
        AFGBuildableDockingStation* Station = IsValid(Dock) ? Dock->GetStation() : nullptr;
        AFGWheeledVehicle* Owner = Vehicle->GetOwnerVehicle();
        const bool bStillDocked = IsValid(Station) && IsValid(Owner) &&
            Station->GetDockedActor() == Owner && Vehicle->IsCurrentlyDocking();
        if (bStillDocked)
        {
            FMyDTTHooks::SetHeld(Station, true, TEXT("restored"));
            State.bParked = true;
            ++Restored;
            UE_LOG(LogDynamicTimeTableSubsystem, Display, TEXT("PARK_RESTORED Truck='%s' Depot='%s'"),
                *DTSVehicleName(Vehicle), *DockDisplayName(State.DepotGuid));
        }
        else
        {
            State.Phase = EDTTTruckPhase::ToDepot;
            State.bParked = false;
            ++Stale;
            UE_LOG(LogDynamicTimeTableSubsystem, Warning, TEXT("PARK_STALE Truck='%s' Depot='%s' Station=%s Docked='%s' Docking=%s"),
                *DTSVehicleName(Vehicle), *DockDisplayName(State.DepotGuid), IsValid(Station) ? TEXT("ok") : TEXT("missing"),
                IsValid(Station) ? *GetNameSafe(Station->GetDockedActor()) : TEXT("<none>"),
                Vehicle->IsCurrentlyDocking() ? TEXT("true") : TEXT("false"));
            // A dock record without the docking flag is the game's half-state; clear it so the
            // truck can dock again.
            if (IsValid(Station) && IsValid(Owner) && Station->GetDockedActor() == Owner && !Station->IsLoadUnloading())
            {
                Station->ForceUndockActor();
            }
        }
        UpdatePoolVehicleName(Vehicle, State);
    }
    UE_LOG(LogDynamicTimeTableSubsystem, Display, TEXT("PARK_RESTORE_DONE Restored=%d Stale=%d Pool=%d Depots=%d"),
        Restored, Stale, PoolTrucks.Num(), DispatchDepotGuids.Num());

    // A managed truck saved while held at a requester (a leftover drain) has no cycle pending and
    // no hold any more: nothing would ever undock it. The game's own docking survives a load only
    // with its cycle, so a docked managed truck whose station is idle is ours to let go.
    for (const FDTTDispatchStation& Stop : DispatchStations)
    {
        AFGDockingStationIdentifier* Dock = Stop.DockingStationIdentifier.Get();
        AFGBuildableDockingStation* Station = IsValid(Dock) ? Dock->GetStation() : nullptr;
        AFGWheeledVehicle* Docked = IsValid(Station) ? Cast<AFGWheeledVehicle>(Station->GetDockedActor()) : nullptr;
        AFGWheeledVehicleIdentifier* Identifier = IsValid(Docked) ? Docked->GetVehicleIdentifier() : nullptr;
        if (!IsValid(Identifier) || Station->IsLoadUnloading() || IsDepotGuid(Stop.DockingPathNodeGuid)) continue;
        if (!PoolTrucks.Contains(Identifier) && !DispatchTruckAnchors.Contains(Identifier)) continue;
        UE_LOG(LogDynamicTimeTableSubsystem, Warning, TEXT("HOLD_STALE Truck='%s' Station='%s' Reason=docked at a requester after load, undocking"),
            *DTSVehicleName(Identifier), *Stop.DisplayName);
        Station->ForceUndockActor();
    }
}

void UDynamicTimeTableSubsystem::UpdatePoolVehicleName(AFGWheeledVehicleIdentifier* Vehicle, const FDTTPoolTruckState& State) const
{
    if (!IsValid(Vehicle)) return;
    FString WantedName;
    switch (State.Phase)
    {
    case EDTTTruckPhase::Parked:
        WantedName = FString::Printf(TEXT("Parked @ %s"), *DockDisplayName(State.DepotGuid));
        break;
    case EDTTTruckPhase::ToDepot:
        WantedName = State.DepotGuid.IsValid()
            ? FString::Printf(TEXT("→ %s"), *DockDisplayName(State.DepotGuid))
            : FString(TEXT("→ depot?"));
        break;
    case EDTTTruckPhase::Assigned:
    {
        const TArray<FGuid>& Route = Vehicle->GetVehicleRoute();
        const int32 Index = Vehicle->GetCurrentTargetWaypointIndex();
        WantedName = Route.IsValidIndex(Index)
            ? FString::Printf(TEXT("%s → %s"), *State.ResourceId, *DockDisplayName(Route[Index]))
            : State.ResourceId;
        break;
    }
    default:
        // Manual: the player's name stays.
        return;
    }
    const FString PreviousName = Vehicle->GetVehicleName().ToString();
    if (PreviousName == WantedName) return;
    Vehicle->SetVehicleName(FText::FromString(WantedName));
    UE_LOG(LogDynamicTimeTableSubsystem, Verbose, TEXT("POOL_TRUCK_RENAMED Previous='%s' New='%s'"), *PreviousName, *WantedName);
}

AFGWheeledVehicleIdentifier* UDynamicTimeTableSubsystem::FindVehicleByQuery(const FString& Query) const
{
    AFGVehicleSubsystem* VehicleSubsystem = AFGVehicleSubsystem::Get(GetWorld());
    if (!IsValid(VehicleSubsystem)) return nullptr;
    const FString Wanted = Query.TrimStartAndEnd();
    if (Wanted.IsEmpty()) return nullptr;
    AFGWheeledVehicleIdentifier* ByDisplayName = nullptr;
    for (AFGWheeledVehicleIdentifier* Vehicle : VehicleSubsystem->GetAllVehicles())
    {
        if (!IsValid(Vehicle)) continue;
        const FString LogName = DTSVehicleName(Vehicle);
        if (LogName.Equals(Wanted, ESearchCase::IgnoreCase)) return Vehicle;
        // The object number alone, "1234" or "#1234".
        int32 Hash = INDEX_NONE;
        const FString Number = LogName.FindLastChar(TEXT('#'), Hash) ? LogName.Mid(Hash + 1) : FString();
        if (!Number.IsEmpty() && (Number == Wanted || (TEXT("#") + Number) == Wanted)) return Vehicle;
        if (!ByDisplayName && Vehicle->GetVehicleName().ToString().Equals(Wanted, ESearchCase::IgnoreCase)) ByDisplayName = Vehicle;
    }
    return ByDisplayName;
}

void UDynamicTimeTableSubsystem::GetDepotStatusLines(TArray<FString>& OutLines) const
{
    OutLines.Reset();
    if (DispatchDepotGuids.Num() == 0)
    {
        OutLines.Add(TEXT("dTT: no depots. Add \"(D)\" to a truck station name to make it one."));
    }
    for (const FGuid& Depot : DispatchDepotGuids)
    {
        const AFGDockingStationIdentifier* Dock = FindDockByPathNode(Depot);
        const AFGBuildableDockingStation* Station = IsValid(Dock) ? Dock->GetStation() : nullptr;
        const TWeakObjectPtr<AFGWheeledVehicleIdentifier>* Claim = DepotClaims.Find(Depot);
        const AFGWheeledVehicleIdentifier* Claimant = Claim ? Claim->Get() : nullptr;
        FString DockedName = TEXT("-");
        if (IsValid(Station))
        {
            if (const AFGWheeledVehicle* Docked = Cast<AFGWheeledVehicle>(Station->GetDockedActor()))
                DockedName = DTSVehicleName(Docked->GetVehicleIdentifier());
        }
        OutLines.Add(FString::Printf(TEXT("Depot '%s': claim=%s docked=%s held=%s"),
            *DockDisplayName(Depot), IsValid(Claimant) ? *DTSVehicleName(Claimant) : TEXT("free"), *DockedName,
            IsValid(Station) && FMyDTTHooks::IsHeld(Station) ? TEXT("yes") : TEXT("no")));
    }
    for (const auto& Pair : PoolTrucks)
    {
        const AFGWheeledVehicleIdentifier* Vehicle = Pair.Key.Get();
        if (!IsValid(Vehicle)) continue;
        const FDTTPoolTruckState& State = Pair.Value;
        OutLines.Add(FString::Printf(TEXT("Truck '%s': %s depot=%s resource=%s autopilot=%s stops=%d"),
            *DTSVehicleName(Vehicle), *UEnum::GetDisplayValueAsText(State.Phase).ToString(),
            State.DepotGuid.IsValid() ? *DockDisplayName(State.DepotGuid) : TEXT("-"),
            State.ResourceId.IsEmpty() ? TEXT("-") : *State.ResourceId,
            Vehicle->IsAutopilotEnabled() ? TEXT("on") : TEXT("off"), Vehicle->GetVehicleRoute().Num()));
    }
    if (PoolTrucks.Num() == 0) OutLines.Add(TEXT("No pool trucks: a truck joins when its route contains a depot, or with /dtt park <truck>."));
}

bool UDynamicTimeTableSubsystem::CommandParkTruck(const FString& TruckQuery, FString& OutMessage)
{
    if (!IsServer()) { OutMessage = TEXT("dTT: only the server can park trucks."); return false; }
    AFGWheeledVehicleIdentifier* Vehicle = FindVehicleByQuery(TruckQuery);
    if (!IsValid(Vehicle)) { OutMessage = FString::Printf(TEXT("dTT: no truck matches '%s' (use the Name#number from /dtt depots or the display name)."), *TruckQuery); return false; }
    if (!Vehicle->IsAutopilotEnabled()) { OutMessage = FString::Printf(TEXT("dTT: '%s' has its autopilot off."), *DTSVehicleName(Vehicle)); return false; }
    if (DispatchDepotGuids.Num() == 0) { OutMessage = TEXT("dTT: there is no depot; add \"(D)\" to a truck station name."); return false; }

    FDTTPoolTruckState* State = PoolTrucks.Find(Vehicle);
    if (State && State->Phase == EDTTTruckPhase::Parked)
    {
        OutMessage = FString::Printf(TEXT("dTT: '%s' is already parked at %s."), *DTSVehicleName(Vehicle), *DockDisplayName(State->DepotGuid));
        return false;
    }
    if (!State)
    {
        const TArray<FGuid>& Route = Vehicle->GetVehicleRoute();
        const int32 Index = Vehicle->GetCurrentTargetWaypointIndex();
        const FGuid From = Route.IsValidIndex(Index) ? Route[Index] : (Route.Num() > 0 ? Route[0] : FGuid());
        const FGuid Depot = FindNearestDepot(Vehicle, From);
        if (!Depot.IsValid())
        {
            OutMessage = FString::Printf(TEXT("dTT: no free %s depot reachable from '%s' (a truck parks only at a depot of its own kind)."),
                IsFluidTruck(Vehicle) ? TEXT("fluid") : TEXT("solid"), *DockDisplayName(From));
            return false;
        }
        State = &JoinPool(Vehicle, Depot, TEXT("command"));
    }
    else
    {
        State->Phase = EDTTTruckPhase::ToDepot;
    }
    ProcessPoolTruckTarget(Vehicle, *State, TEXT("command park"));
    OutMessage = State->DepotGuid.IsValid()
        ? FString::Printf(TEXT("dTT: '%s' is heading for %s."), *DTSVehicleName(Vehicle), *DockDisplayName(State->DepotGuid))
        : FString::Printf(TEXT("dTT: '%s' joined the pool but no depot is free right now."), *DTSVehicleName(Vehicle));
    return State->DepotGuid.IsValid();
}

bool UDynamicTimeTableSubsystem::CommandReleaseTruck(const FString& TruckQuery, FString& OutMessage)
{
    if (!IsServer()) { OutMessage = TEXT("dTT: only the server can release trucks."); return false; }
    AFGWheeledVehicleIdentifier* Vehicle = FindVehicleByQuery(TruckQuery);
    if (!IsValid(Vehicle)) { OutMessage = FString::Printf(TEXT("dTT: no truck matches '%s'."), *TruckQuery); return false; }
    FDTTPoolTruckState* State = PoolTrucks.Find(Vehicle);
    if (!State || State->Phase != EDTTTruckPhase::Parked)
    {
        OutMessage = FString::Printf(TEXT("dTT: '%s' is not parked."), *DTSVehicleName(Vehicle));
        return false;
    }
    const TArray<FGuid> Loop{ State->DepotGuid, State->DepotGuid };
    if (!ReleaseParkedTruck(Vehicle, *State, Loop, EDTTTruckPhase::ToDepot, TEXT("command release")))
    {
        OutMessage = FString::Printf(TEXT("dTT: could not release '%s' (see the log)."), *DTSVehicleName(Vehicle));
        return false;
    }
    OutMessage = FString::Printf(TEXT("dTT: '%s' released; it drives one loop and parks again at %s."), *DTSVehicleName(Vehicle), *DockDisplayName(State->DepotGuid));
    return true;
}

// ---------------------------------------------------------------------------
// Rows for the Dispatch and Trucks tabs, the chat listings and the RCO snapshot. Built here so
// that every view shows the same thing; formatting is static so the client can format a
// snapshot row without a subsystem.
// ---------------------------------------------------------------------------

FString UDynamicTimeTableSubsystem::PhaseLabel(EDTTTruckPhase Phase)
{
    switch (Phase)
    {
    case EDTTTruckPhase::ToDepot: return TEXT("ToDepot");
    case EDTTTruckPhase::Parked: return TEXT("Parked");
    case EDTTTruckPhase::Assigned: return TEXT("Assigned");
    case EDTTTruckPhase::Manual: return TEXT("Manual");
    default: return TEXT("Unmanaged");
    }
}

bool UDynamicTimeTableSubsystem::IsRequesterOpen(const FDTTDispatchStation& Station, TSubclassOf<UFGItemDescriptor> ItemClass) const
{
    if (!ItemClass || GetEffectiveDispatchRole(Station) != EDTTDispatchRole::Requester || !DoesDispatchStationMatchMode(Station)) return false;
    const int32 Stock = ReadDockStockInStacks(Station.DockingStationIdentifier.Get(), ItemClass);
    FDTTRequestLevels Levels;
    return Stock >= 0 && GetRequestLevels(Station, ItemClass, Levels) && Levels.Trigger > 0 && Stock < Levels.Trigger;
}

void UDynamicTimeTableSubsystem::UpdateRequesterWaitTimes()
{
    UWorld* World = GetWorld();
    const double Now = IsValid(World) ? World->GetTimeSeconds() : 0.0;
    TSet<FGuid> Waiting;
    for (const FDTTDispatchStation& Station : DispatchStations)
    {
        if (!Station.DockingPathNodeGuid.IsValid() || RequesterClaims.Contains(Station.DockingPathNodeGuid)) continue;
        if (!IsRequesterOpen(Station, ResolveItemClassForResource(Station.ResourceId))) continue;
        Waiting.Add(Station.DockingPathNodeGuid);
        const double Since = RequesterOpenSinceWorldSeconds.FindOrAdd(Station.DockingPathNodeGuid, Now);
        const int32 WaitingSeconds = FMath::Max(0, FMath::RoundToInt(Now - Since));
        if (Settings.OverdueRequestMinutes > 0 && WaitingSeconds >= Settings.OverdueRequestMinutes * 60 &&
            !OverdueRequestsLogged.Contains(Station.DockingPathNodeGuid))
        {
            OverdueRequestsLogged.Add(Station.DockingPathNodeGuid);
            UE_LOG(LogDynamicTimeTableSubsystem, Warning, TEXT("DISPATCH_REQUEST_OVERDUE Station=\"%s\" Resource=\"%s\" WaitingSeconds=%d Limit=%dm"),
                *Station.DisplayName, *Station.ResourceId, WaitingSeconds, Settings.OverdueRequestMinutes);
        }
    }
    // Closed or claimed: the episode is over, the next one starts from zero and may be logged again.
    for (auto It = RequesterOpenSinceWorldSeconds.CreateIterator(); It; ++It)
        if (!Waiting.Contains(It.Key())) { OverdueRequestsLogged.Remove(It.Key()); It.RemoveCurrent(); }
}

void UDynamicTimeTableSubsystem::BuildDispatchRows(TArray<FDTTDispatchRow>& OutRows, const FString& ResourceFilter) const
{
    OutRows.Reset();
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
        const TSubclassOf<UFGItemDescriptor> ItemClass = ResolveItemClassForResource(Station.ResourceId);

        FDTTDispatchRow& Row = OutRows.AddDefaulted_GetRef();
        Row.ResourceId = Station.ResourceId;
        Row.ItemClass = TSoftClassPtr<UFGItemDescriptor>(ItemClass.Get());
        Row.StationName = StripDispatchTag(Station.DisplayName);
        Row.DockingPathNodeGuid = Station.DockingPathNodeGuid;
        Row.Role = GetEffectiveDispatchRole(Station);
        Row.bModeKnown = IsValid(Dock);
        Row.bLoadMode = IsValid(Dock) && Dock->GetIsInLoadMode();
        Row.bModeMatches = DoesDispatchStationMatchMode(Station);
        Row.StockStacks = ItemClass ? ReadDockStockInStacks(Station.DockingStationIdentifier.Get(), ItemClass) : INDEX_NONE;
        FDTTRequestLevels Levels;
        if (GetRequestLevels(Station, ItemClass, Levels))
        {
            Row.TriggerStacks = Levels.Trigger;
            Row.ThresholdStacks = Levels.Target;
            Row.CapacityStacks = Levels.Capacity;
            Row.bFluid = Levels.bFluid;
        }
        if (Row.Role == EDTTDispatchRole::Producer) continue;

        Row.bOpen = Row.bModeMatches && Row.StockStacks >= 0 && Row.TriggerStacks > 0 && Row.StockStacks < Row.TriggerStacks;
        if (const double* Since = RequesterOpenSinceWorldSeconds.Find(Station.DockingPathNodeGuid))
        {
            const UWorld* World = GetWorld();
            if (IsValid(World)) Row.WaitingSeconds = FMath::Max(0, FMath::RoundToInt(World->GetTimeSeconds() - *Since));
        }
        if (const TWeakObjectPtr<AFGWheeledVehicleIdentifier>* Owner = RequesterClaims.Find(Station.DockingPathNodeGuid))
        {
            AFGWheeledVehicleIdentifier* Vehicle = Owner->Get();
            Row.ClaimOwner = IsValid(Vehicle) ? DTSVehicleName(Vehicle) : TEXT("<invalid vehicle>");
            // What the claiming truck's lap planned to leave here, in the row's stacks unit.
            const int32* PlannedItems = nullptr;
            if (const FDTTPoolTruckState* Pool = PoolTrucks.Find(Vehicle)) PlannedItems = Pool->PlannedItems.Find(Station.DockingPathNodeGuid);
            else if (const FDTTDispatchTruckAnchors* Anchors = DispatchTruckAnchors.Find(Vehicle)) PlannedItems = Anchors->PlannedItems.Find(Station.DockingPathNodeGuid);
            if (PlannedItems && ItemClass)
            {
                Row.PlannedStacks = *PlannedItems / FMath::Max(1, UFGItemDescriptor::GetStackSize(ItemClass));
            }
        }
    }

    // Depots and fuel stops belong to no resource, so a per-resource listing leaves them out.
    if (!ResourceFilter.IsEmpty()) return;
    TArray<FGuid> Depots = DispatchDepotGuids.Array();
    Depots.Sort([](const FGuid& A, const FGuid& B) { return A.ToString() < B.ToString(); });
    for (const FGuid& Depot : Depots)
    {
        const AFGDockingStationIdentifier* Dock = FindDockByPathNode(Depot);
        const AFGBuildableDockingStation* Station = IsValid(Dock) ? Dock->GetStation() : nullptr;
        FDTTDispatchRow& Row = OutRows.AddDefaulted_GetRef();
        Row.bDepot = true;
        Row.StationName = DockDisplayName(Depot);
        Row.DockingPathNodeGuid = Depot;
        Row.bModeKnown = IsValid(Station);
        Row.bModeMatches = true;
        if (const TWeakObjectPtr<AFGWheeledVehicleIdentifier>* Claim = DepotClaims.Find(Depot))
        {
            Row.ClaimOwner = Claim->IsValid() ? DTSVehicleName(Claim->Get()) : TEXT("<invalid vehicle>");
        }
        FString Docked = TEXT("-");
        if (IsValid(Station))
        {
            if (const AFGWheeledVehicle* DockedVehicle = Cast<AFGWheeledVehicle>(Station->GetDockedActor()))
                Docked = DTSVehicleName(DockedVehicle->GetVehicleIdentifier());
        }
        Row.Note = FString::Printf(TEXT("docked=%s held=%s"), *Docked,
            IsValid(Station) && FMyDTTHooks::IsHeld(Station) ? TEXT("yes") : TEXT("no"));
    }
    TArray<FGuid> FuelStops = DispatchFuelStopGuids.Array();
    FuelStops.Sort([](const FGuid& A, const FGuid& B) { return A.ToString() < B.ToString(); });
    for (const FGuid& FuelGuid : FuelStops)
    {
        const AFGDockingStationIdentifier* FuelDock = FindDockByPathNode(FuelGuid);
        FDTTDispatchRow& Row = OutRows.AddDefaulted_GetRef();
        Row.bFuelStop = true;
        Row.StationName = IsValid(FuelDock) ? FuelDock->GetStationName().ToString() : TEXT("<missing dock>");
        Row.DockingPathNodeGuid = FuelGuid;
        Row.bModeKnown = IsValid(FuelDock);
        Row.bModeMatches = true;
        Row.Note = FString::Printf(TEXT("guid=%s"), *FuelGuid.ToString());
    }
}

FString UDynamicTimeTableSubsystem::FormatDispatchRow(const FDTTDispatchRow& Row)
{
    if (Row.bDepot)
    {
        return FString::Printf(TEXT("Depot (D) | %s | claim=%s | %s"),
            *Row.StationName, Row.ClaimOwner.IsEmpty() ? TEXT("free") : *Row.ClaimOwner, *Row.Note);
    }
    if (Row.bFuelStop)
    {
        return FString::Printf(TEXT("Fuel (F) | %s | %s"), *Row.StationName, *Row.Note);
    }
    const TCHAR* ActualMode = !Row.bModeKnown ? TEXT("unavailable") : (Row.bLoadMode ? TEXT("LOAD") : TEXT("UNLOAD"));
    if (Row.Role == EDTTDispatchRole::Producer)
    {
        return FString::Printf(TEXT("%s | Producer | %s | mode=%s active=%s | stock=%s"),
            *Row.ResourceId, *Row.StationName, ActualMode, Row.bModeMatches ? TEXT("yes") : TEXT("no"),
            Row.StockStacks >= 0 ? *FString::Printf(TEXT("%d stacks"), Row.StockStacks) : TEXT("unknown"));
    }
    // stock=S call<T fill=F cap=C: called while the stock is below T, filled up to F.
    const FString Trigger = Row.TriggerStacks >= 0
        ? (Row.bFluid && Row.CapacityStacks > 0
            ? FString::Printf(TEXT("%d (%d%%)"), Row.TriggerStacks, FMath::RoundToInt(100.0f * Row.TriggerStacks / Row.CapacityStacks))
            : FString::FromInt(Row.TriggerStacks))
        : TEXT("?");
    // waiting=Nm: open with nobody claiming it for N minutes (the window colours it by the setting).
    const FString Waiting = Row.WaitingSeconds >= 60 ? FString::Printf(TEXT(" waiting=%dm"), Row.WaitingSeconds / 60) : FString();
    return FString::Printf(TEXT("%s | Requester | %s | mode=%s active=%s | stock=%s call<%s fill=%s cap=%s open=%s%s | claim=%s%s"),
        *Row.ResourceId, *Row.StationName, ActualMode, Row.bModeMatches ? TEXT("yes") : TEXT("no"),
        Row.StockStacks >= 0 ? *FString::FromInt(Row.StockStacks) : TEXT("unknown"),
        *Trigger,
        Row.ThresholdStacks >= 0 ? *FString::FromInt(Row.ThresholdStacks) : TEXT("?"),
        Row.CapacityStacks >= 0 ? *FString::FromInt(Row.CapacityStacks) : TEXT("?"),
        Row.bOpen ? TEXT("yes") : TEXT("no"), *Waiting,
        Row.ClaimOwner.IsEmpty() ? TEXT("free") : *Row.ClaimOwner,
        Row.PlannedStacks > 0 ? *FString::Printf(TEXT(" planned=%d"), Row.PlannedStacks) : TEXT(""));
}

void UDynamicTimeTableSubsystem::BuildTruckRows(TArray<FDTTTruckRow>& OutRows, const FString& ResourceFilter) const
{
    OutRows.Reset();
    auto FillCommon = [&](FDTTTruckRow& Row, AFGWheeledVehicleIdentifier* Vehicle, TSubclassOf<UFGItemDescriptor> ItemClass)
    {
        Row.Name = DTSVehicleName(Vehicle);
        Row.ItemClass = TSoftClassPtr<UFGItemDescriptor>(ItemClass.Get());
        TSubclassOf<UFGItemDescriptor> CargoClass;
        DescribeCargo(Vehicle, &CargoClass);
        Row.CargoItemClass = TSoftClassPtr<UFGItemDescriptor>(CargoClass.Get());
        const TArray<FGuid>& Route = Vehicle->GetVehicleRoute();
        for (const FGuid& Node : Route) Row.RouteStops.Add(DockDisplayName(Node));
        Row.TargetIndex = Vehicle->GetCurrentTargetWaypointIndex();
        Row.bAutopilot = Vehicle->IsAutopilotEnabled();
        Row.bDocking = Vehicle->IsCurrentlyDocking();
        if (ItemClass)
        {
            int32 Capacity = 0, OnBoard = 0;
            if (EstimateTruckCargoStacks(Vehicle, ItemClass, Capacity, OnBoard))
            {
                Row.CargoStacks = OnBoard;
                Row.CargoCapacityStacks = Capacity;
            }
        }
        else
        {
            // No resource to count in: occupied slots against the slot count.
            AFGWheeledVehicle* WheeledVehicle = Vehicle->GetOwnerVehicle();
            UFGInventoryComponent* Storage = IsValid(WheeledVehicle) ? WheeledVehicle->GetStorageInventory() : nullptr;
            if (IsValid(Storage) && Storage->GetSizeLinear() > 0)
            {
                TArray<FInventoryStack> Stacks;
                Storage->GetInventoryStacks(Stacks);
                Row.CargoStacks = Stacks.Num();
                Row.CargoCapacityStacks = Storage->GetSizeLinear();
            }
        }
    };
    auto PlannedSummary = [&](const TMap<FGuid, int32>& Planned, TSubclassOf<UFGItemDescriptor> ItemClass) -> FString
    {
        const int32 StackSize = ItemClass ? FMath::Max(1, UFGItemDescriptor::GetStackSize(ItemClass)) : 1;
        TArray<FString> Parts;
        for (const auto& Pair : Planned)
        {
            Parts.Add(FString::Printf(TEXT("%s:%d"), *DockDisplayName(Pair.Key), Pair.Value / StackSize));
        }
        Parts.Sort();
        return FString::Join(Parts, TEXT(", "));
    };

    for (const auto& Pair : DispatchTruckAnchors)
    {
        AFGWheeledVehicleIdentifier* Vehicle = Pair.Key.Get();
        if (!IsValid(Vehicle) || PoolTrucks.Contains(Vehicle)) continue;
        const TArray<FGuid>& Route = Vehicle->GetVehicleRoute();
        const FString ResourceId = Route.Num() > 0 ? ResolveDispatchResourceFromProducer(Route[0]) : FString();
        if (ResourceId.IsEmpty()) continue;
        if (!ResourceFilter.IsEmpty() && !ResourceId.Equals(ResourceFilter, ESearchCase::IgnoreCase)) continue;
        const TSubclassOf<UFGItemDescriptor> ItemClass = ResolveItemClassForResource(ResourceId);
        FDTTTruckRow& Row = OutRows.AddDefaulted_GetRef();
        Row.bPool = false;
        Row.Phase = EDTTTruckPhase::Unmanaged;
        Row.ResourceId = ResourceId;
        FillCommon(Row, Vehicle, ItemClass);
        Row.PlannedSummary = PlannedSummary(Pair.Value.PlannedItems, ItemClass);
    }
    for (const auto& Pair : PoolTrucks)
    {
        AFGWheeledVehicleIdentifier* Vehicle = Pair.Key.Get();
        if (!IsValid(Vehicle)) continue;
        const FDTTPoolTruckState& State = Pair.Value;
        if (!ResourceFilter.IsEmpty() && !State.ResourceId.Equals(ResourceFilter, ESearchCase::IgnoreCase)) continue;
        const TSubclassOf<UFGItemDescriptor> ItemClass = State.ResourceId.IsEmpty() ? nullptr : ResolveItemClassForResource(State.ResourceId);
        FDTTTruckRow& Row = OutRows.AddDefaulted_GetRef();
        Row.bPool = true;
        Row.Phase = State.Phase;
        Row.ResourceId = State.ResourceId;
        Row.DepotName = State.DepotGuid.IsValid() ? DockDisplayName(State.DepotGuid) : FString();
        FillCommon(Row, Vehicle, ItemClass);
        Row.PlannedSummary = State.Phase == EDTTTruckPhase::Assigned ? PlannedSummary(State.PlannedItems, ItemClass) : FString();
        TSubclassOf<UFGItemDescriptor> SingleClass;
        Row.CargoNote = DescribeCargo(Vehicle, &SingleClass);
        if (!Row.CargoNote.IsEmpty())
        {
            if (State.Phase == EDTTTruckPhase::Parked)
            {
                Row.CargoNote += SingleClass
                    ? FString::Printf(TEXT(", waits for %s"), *UFGItemDescriptor::GetItemName(SingleClass).ToString())
                    : FString(TEXT(", mixed cargo blocks dispatch"));
            }
            else if (ItemClass && SingleClass != ItemClass)
            {
                Row.CargoNote += TEXT(", foreign cargo");
            }
        }
    }
    OutRows.Sort([](const FDTTTruckRow& A, const FDTTTruckRow& B)
    {
        if (A.bPool != B.bPool) return !A.bPool;
        return A.Name < B.Name;
    });
}

FString UDynamicTimeTableSubsystem::FormatTruckRow(const FDTTTruckRow& Row)
{
    const FString Cargo = Row.CargoCapacityStacks >= 0
        ? FString::Printf(TEXT("%d/%d stacks"), Row.CargoStacks, Row.CargoCapacityStacks) : FString(TEXT("unknown"));
    const FString Route = FString::Join(Row.RouteStops, TEXT(" > "));
    if (!Row.bPool)
    {
        return FString::Printf(TEXT("%s | Truck | %s | route=[%s] target=%d%s | cargo=%s%s"),
            *Row.ResourceId, *Row.Name, *Route, Row.TargetIndex, Row.bAutopilot ? TEXT("") : TEXT(" autopilot=off"), *Cargo,
            Row.PlannedSummary.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" | planned=%s"), *Row.PlannedSummary));
    }
    return FString::Printf(TEXT("%s | Pool %s | %s | depot=%s | route=[%s] target=%d%s%s | cargo=%s%s%s"),
        Row.ResourceId.IsEmpty() ? TEXT("-") : *Row.ResourceId, *PhaseLabel(Row.Phase), *Row.Name,
        Row.DepotName.IsEmpty() ? TEXT("-") : *Row.DepotName, *Route, Row.TargetIndex,
        Row.bAutopilot ? TEXT("") : TEXT(" autopilot=off"), Row.bDocking ? TEXT(" docked") : TEXT(""), *Cargo,
        Row.CargoNote.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" [%s]"), *Row.CargoNote),
        Row.PlannedSummary.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" | planned=%s"), *Row.PlannedSummary));
}

void UDynamicTimeTableSubsystem::GetTruckStatusLines(TArray<FString>& OutLines, const FString& ResourceFilter) const
{
    OutLines.Reset();
    TArray<FDTTTruckRow> Rows;
    BuildTruckRows(Rows, ResourceFilter);
    int32 Pool = 0;
    for (const FDTTTruckRow& Row : Rows) if (Row.bPool) ++Pool;
    OutLines.Add(FString::Printf(TEXT("dTT trucks: dispatch=%d pool=%d depots=%d"), Rows.Num() - Pool, Pool, DispatchDepotGuids.Num()));
    for (const FDTTTruckRow& Row : Rows) OutLines.Add(FormatTruckRow(Row));
    if (Rows.Num() == 0)
    {
        OutLines.Add(TEXT("No managed trucks. A truck whose first stop is a (Resource) LOAD station runs dispatch laps; "
            "one whose route contains a (D) depot joins the pool."));
    }
}
