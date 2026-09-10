#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "DynamicTimeTableTypes.h"
#include "DynamicTimeTableSubsystem.generated.h"

class AFGTrain;
class AFGBuildableRailroadStation;
class AFGTrainStationIdentifier;
class AFGRailroadTimeTable;
class AFGDockingStationIdentifier;
class AFGWheeledVehicleIdentifier;
class ADTTSaveDataActor;

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FDTTGroupsChangedDelegate);
DECLARE_MULTICAST_DELEGATE(FDTTGroupsChangedNativeDelegate);

UCLASS(BlueprintType)
class MYDTT_API UDynamicTimeTableSubsystem : public UWorldSubsystem
{
    GENERATED_BODY()

public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;
    virtual void OnWorldBeginPlay(UWorld& InWorld) override;

    void HandleTrainDocked(AFGTrain* Train, AFGBuildableRailroadStation* DockedStation);
    void HandleTimeTableChanged(AFGRailroadTimeTable* TimeTable);
    void HandleSelfDrivingChanged(AFGTrain* Train, bool bEnabled);
    void HandleStationNameChanged(AFGTrainStationIdentifier* Identifier);

    // Truck counterparts of the train events above. There is no hook for "the autopilot picked
    // its next waypoint" or "the truck docked": those setters carry no UFUNCTION and the game
    // does not call them through an address a hook can reach - measured in game, the handlers
    // never fired once across 17 waypoint changes. Those two signals are polled instead, see
    // PollTruckStates.
    void HandleVehicleRouteChanged(AFGWheeledVehicleIdentifier* Vehicle);
    void HandleAutopilotChanged(AFGWheeledVehicleIdentifier* Vehicle, bool bEnabled);
    void HandleDockingStationNameChanged(AFGDockingStationIdentifier* Identifier);

    UFUNCTION(BlueprintCallable, Category = "Dynamic Time Table|Groups") FGuid CreateGroup(const FString& GroupName, EDTTGroupKind Kind = EDTTGroupKind::Train);
    UFUNCTION(BlueprintCallable, Category = "Dynamic Time Table|Groups") bool RemoveGroup(FGuid GroupId);
    UFUNCTION(BlueprintCallable, Category = "Dynamic Time Table|Groups") bool RenameGroup(FGuid GroupId, const FString& NewName);
    UFUNCTION(BlueprintCallable, Category = "Dynamic Time Table|Groups") bool AddStationByName(FGuid GroupId, const FString& StationName, int32 TargetSlots = 1);
    UFUNCTION(BlueprintCallable, Category = "Dynamic Time Table|Groups") bool RemoveStationByName(FGuid GroupId, const FString& StationName);
    UFUNCTION(BlueprintCallable, Category = "Dynamic Time Table|Groups") bool SetTargetSlots(FGuid GroupId, const FString& StationName, int32 TargetSlots);
    UFUNCTION(BlueprintCallable, Category = "Dynamic Time Table|Groups") void GetAllVanillaStationNames(TArray<FString>& OutStationNames) const;
    UFUNCTION(BlueprintCallable, Category = "Dynamic Time Table|Groups") void GetAllDockingStationNames(TArray<FString>& OutStationNames) const;
    UFUNCTION(BlueprintPure, Category = "Dynamic Time Table|Groups") const TArray<FDTTStationGroup>& GetStationGroups() const;
    /** Human-readable server-authoritative resource-dispatch diagnostics for /dtt dispatch. */
    void GetDispatchStatusLines(TArray<FString>& OutLines) const;
    UFUNCTION(BlueprintCallable, Category = "Dynamic Time Table|Groups") void GetGroupRuntimeLoad(FGuid GroupId, TArray<FDTTStationRuntimeLoad>& OutLoads) const;
    UFUNCTION(BlueprintCallable, Category = "Dynamic Time Table|Runtime") void RecalculateBookingsFromVanilla();
    int32 GetConfigurationRevision() const { return ConfigurationRevision; }

    /**
     * The group a station belongs to is written in the station's own name, as a trailing tag:
     * "Ore Unload 1 [ore]" or "Ore Unload 1 [ore,3]" - group "ore", three slots. That makes the
     * station name the single source of truth: the game already saves and replicates it, so the
     * mod keeps no configuration of its own and cannot go out of sync with the world.
     *
     * Returns false when the name carries no usable tag; OutBaseName is then the name unchanged.
     */
    static bool ParseStationTag(const FString& FullName, FString& OutBaseName, FString& OutGroupName, int32& OutTargetSlots);
    /** The name without its tag, for display. */
    static FString StripStationTag(const FString& FullName);
    /** The inverse of ParseStationTag. Slots are written only when above the default of one. */
    static FString MakeTaggedName(const FString& BaseName, const FString& GroupName, int32 TargetSlots);
    /** Strips the characters that would break the tag syntax out of a group name. */
    static FString SanitizeGroupName(const FString& GroupName);

    /**
     * A resource is written in a *separate* tag, e.g. "Iron Mine (Iron)". LOAD makes it a producer,
     * UNLOAD a requester with the default threshold. "(Iron-10)" overrides that requester threshold;
     * legacy explicit "(Iron+)" remains supported. Parentheses deliberately differ from the
     * "[group,N]" balancing tag, so one station can carry both independently.
     */
    static bool ParseDispatchTag(const FString& FullName, FString& OutBaseName, FString& OutResourceId, EDTTDispatchRole& OutRole, int32& OutThreshold);
    /** The name without its dispatch tag, for display. */
    static FString StripDispatchTag(const FString& FullName);
    /** The inverse of ParseDispatchTag. */
    static FString MakeDispatchTaggedName(const FString& BaseName, const FString& ResourceId, EDTTDispatchRole Role, int32 Threshold);
    /** Accepts either "Resource" or a whole-name neutral wrapper "(Resource)" on a vehicle. */
    static FString NormalizeDispatchResourceId(const FString& VehicleName);
    /** Exact, resource-independent marker for an optional dedicated refuelling waypoint. */
    static bool ParseFuelStopTag(const FString& FullName, FString& OutBaseName);

    UPROPERTY(BlueprintAssignable, Category = "Dynamic Time Table")
    FDTTGroupsChangedDelegate OnGroupsChanged;

    FDTTGroupsChangedNativeDelegate OnGroupsChangedNative;

private:
    // UPROPERTY is load-bearing, not decoration: a TObjectPtr inside a plain member container is
    // an ordinary raw pointer with no GC awareness, so IsValid() on a dismantled station would
    // read freed memory. Under UPROPERTY the reference is nulled for us instead.
    UPROPERTY()
    TArray<FDTTStationGroup> StationGroups;
    // Weak on both sides for the same reason - a booking outlives the actors it names.
    TMap<TWeakObjectPtr<AFGTrain>, TWeakObjectPtr<AFGTrainStationIdentifier>> ActiveBookings;
    TMap<TWeakObjectPtr<AFGWheeledVehicleIdentifier>, TWeakObjectPtr<AFGDockingStationIdentifier>> TruckBookings;
    TWeakObjectPtr<ADTTSaveDataActor> SaveDataActor;
    bool bPersistenceReady = false;
    /**
     * Groups a group name the player typed in the window before any station carries its tag.
     * A group only really exists through the stations tagged with it, so an empty one has nowhere
     * to live - this keeps it visible for the rest of the session and is deliberately not saved.
     */
    TMap<FGuid, FDTTStationGroup> DraftGroups;
    /** Order-independent hash over every station name, to notice a build or a demolish cheaply. */
    uint32 LastStationFingerprint = 0;
    bool bStationFingerprintValid = false;
    /** Renames this mod makes itself, so the rename hook does not treat them as player edits. */
    TSet<AActor*> InternalNameChanges;
    int32 MigrationAttempts = 0;
    /**
     * Raised by a timer a few seconds after persistence is initialised. At OnWorldBeginPlay the
     * railroad and vehicle subsystems have not registered their identifiers yet, so nothing that
     * reads the world's stations may run before this.
     */
    bool bWorldStationsReady = false;
    /** Group -> PriorityIndex of the station picked last. Runtime only, deliberately not saved. */
    TMap<FGuid, int32> GroupRotationCursor;
    int32 ConfigurationRevision = 0;
    TSet<AFGRailroadTimeTable*> InternalTimeTableChanges;
    TSet<AFGWheeledVehicleIdentifier*> InternalRouteChanges;
    TSet<TWeakObjectPtr<AFGRailroadTimeTable>> PendingTimeTableReevaluations;
    TSet<TWeakObjectPtr<AFGWheeledVehicleIdentifier>> PendingRouteReevaluations;
    bool bTimeTableReevaluationScheduled = false;
    bool bRouteReevaluationScheduled = false;

    /** What the last poll saw for one vehicle. A change in either field is a signal to act on. */
    struct FDTTTruckPollState
    {
        int32 TargetIndex = INDEX_NONE;
        bool bDocking = false;
    };
    TMap<TWeakObjectPtr<AFGWheeledVehicleIdentifier>, FDTTTruckPollState> TruckPollStates;
    FTimerHandle TruckPollTimerHandle;

    // Resource dispatch (producer/requester). Rebuilt alongside StationGroups in
    // RebuildGroupsFromNames; nothing here is SaveGame, same philosophy as the rest of this mod.
    UPROPERTY()
    TArray<FDTTDispatchStation> DispatchStations;
    /** Dock GUIDs carrying the exact reserved marker "(F)"; LOAD/UNLOAD does not matter. */
    TSet<FGuid> DispatchFuelStopGuids;
    // Which truck currently owns a requester stop, keyed by the requester's stable dock path-node
    // GUID (survives a rename, unlike the array index). Released on docking, pruned on timeout/
    // despawn/retag - see PruneStaleRequesterClaims and the cleanup in RebuildGroupsFromNames.
    TMap<FGuid, TWeakObjectPtr<AFGWheeledVehicleIdentifier>> RequesterClaims;
    TMap<FGuid, double> RequesterClaimedAtWorldSeconds;
    static constexpr double RequesterClaimTimeoutSeconds = 600.0;
    // Built once from UFGBlueprintFunctionLibrary::GetAllDescriptorsSorted, not per poll tick.
    mutable TMap<FString, TSubclassOf<class UFGItemDescriptor>> ItemClassByNameCache;
    // Producer is always Route[0]. A dedicated fuel anchor is optional and comes only from an
    // explicit (F) tag; it is remembered because a skipped lap removes it from the live route.
    TMap<TWeakObjectPtr<AFGWheeledVehicleIdentifier>, FDTTDispatchTruckAnchors> DispatchTruckAnchors;

    void InitializePersistence();
    void ScheduleWorldStationsReady();
    void RegisterDTTChatCommand();
    /** Rebuilds every group from the station names currently in the world. */
    void RebuildGroupsFromNames();
    /** Rebuilds only when a station name, or the number of stations, actually changed. */
    void RebuildGroupsIfStationsChanged(const TCHAR* Trigger);
    uint32 ComputeStationFingerprint() const;
    /** False until the railroad and vehicle subsystems have handed out their identifiers. */
    bool HasAnyStations() const;
    /** One-off: turns groups saved by an older version into tags on the station names. */
    void MigrateSavedGroupsToTags();
    bool RenameStationGuarded(AFGTrainStationIdentifier* Identifier, const FString& NewName);
    bool RenameDockGuarded(AFGDockingStationIdentifier* Identifier, const FString& NewName);
    /** Rewrites one station's tag, whichever kind it is. Empty GroupName strips the tag. */
    bool RetagStation(const FDTTStationGroup& Group, const FDTTStationEntry& Entry, const FString& NewGroupName, int32 NewTargetSlots);
    /** Group by id, looking among the drafts too. */
    const FDTTStationGroup* FindGroupOrDraft(FGuid GroupId) const;
    void SyncConfigurationToSaveActor();
    bool IsRuntimeGameWorld() const;
    bool IsServer() const;

    FDTTStationGroup* FindGroup(FGuid GroupId);
    const FDTTStationGroup* FindGroup(FGuid GroupId) const;
    FDTTStationGroup* FindGroupByStation(AFGTrainStationIdentifier* Identifier);
    const FDTTStationGroup* FindGroupByStation(AFGTrainStationIdentifier* Identifier) const;
    FDTTStationEntry* FindStationEntry(FDTTStationGroup& Group, const FString& StationName);
    const FDTTStationEntry* FindStationEntry(const FDTTStationGroup& Group, const FString& StationName) const;
    AFGTrainStationIdentifier* FindVanillaStationByName(const FString& StationName) const;
    void RemoveInvalidBookings();
    void ReleaseBookingIfReached(AFGTrain* Train, AFGTrainStationIdentifier* DockedIdentifier);
    void ReleaseBooking(AFGTrain* Train, const TCHAR* Reason);
    AFGTrain* FindTrainByTimeTable(AFGRailroadTimeTable* TimeTable) const;
    void SchedulePendingTimeTableReevaluation();
    void ProcessPendingTimeTableReevaluations();
    void ProcessCurrentTarget(AFGTrain* Train, const TCHAR* Trigger);
    void RefreshPriorityIndices(FDTTStationGroup& Group);
    void ConfigurationChanged(bool bRecalculateBookings = true);
    FDTTStationEntry* SelectBestStation(FDTTStationGroup& Group);
    int32 GetAssignedCount(AFGTrainStationIdentifier* Identifier) const;
    int32 GetEntryAssignedCount(const FDTTStationGroup& Group, const FDTTStationEntry& Entry) const;
    int32 CountStopsInGroup(AFGRailroadTimeTable* TimeTable, const FDTTStationGroup& Group) const;
    bool ReplaceCurrentStop(AFGRailroadTimeTable* TimeTable, int32 CurrentStop, AFGTrainStationIdentifier* NewIdentifier);

    // Truck side. Mirrors the train helpers above one for one; the balancing rule itself is shared.
    FDTTStationGroup* FindGroupByDock(AFGDockingStationIdentifier* Identifier);
    const FDTTStationGroup* FindGroupByDock(AFGDockingStationIdentifier* Identifier) const;
    AFGDockingStationIdentifier* FindDockByName(const FString& StationName) const;
    AFGDockingStationIdentifier* FindDockByPathNode(const FGuid& PathNodeGuid) const;
    void RemoveInvalidTruckBookings();
    void ReleaseTruckBooking(AFGWheeledVehicleIdentifier* Vehicle, const TCHAR* Reason);
    void StartTruckPolling();
    void PollTruckStates();
    void SchedulePendingRouteReevaluation();
    void ProcessPendingRouteReevaluations();
    void ProcessCurrentTruckTarget(AFGWheeledVehicleIdentifier* Vehicle, const TCHAR* Trigger);
    int32 GetDockAssignedCount(AFGDockingStationIdentifier* Identifier) const;
    int32 CountWaypointsInGroup(AFGWheeledVehicleIdentifier* Vehicle, const FDTTStationGroup& Group) const;
    bool ReplaceCurrentWaypoint(AFGWheeledVehicleIdentifier* Vehicle, int32 WaypointIndex, const FGuid& NewPathNodeGuid);
    /** Path network the group's vehicles travel, or INDEX_NONE when nothing in the group is moving yet. Diagnostics only. */
    int32 GetGroupPathNetworkID(const FDTTStationGroup& Group) const;

    // Resource dispatch.
    static constexpr int32 DefaultDispatchRequesterThresholdStacks = 10;
    /** Resolves a neutral tag against the station's live LOAD/UNLOAD switch. */
    EDTTDispatchRole GetEffectiveDispatchRole(const FDTTDispatchStation& Station) const;
    /** LOAD is producer, UNLOAD is requester; a contradictory tag is never actionable. */
    bool DoesDispatchStationMatchMode(const FDTTDispatchStation& Station) const;
    /** True when at least one live dispatch station uses this resource id. */
    bool IsDispatchTruck(const FString& ResourceId) const;
    /**
     * Producer is structurally Route[0]. An optional dedicated Fuel stop is selected only by its
     * explicit (F) station tag and remembered with its route position while a lap omits it.
     */
    bool UpdateRememberedAnchors(AFGWheeledVehicleIdentifier* Vehicle, const FString& ResourceId, const TArray<FGuid>& Route);
    /** Rewrites the complete route while suppressing this mod's SetVehicleRoute hook. */
    bool ReplaceRoute(AFGWheeledVehicleIdentifier* Vehicle, const TArray<FGuid>& NewRoute);
    /** Fuel cost for one directed edge in the vehicle path network, or a negative value on failure. */
    float SumSegmentFuelConsumption(class AFGVehicleSubsystem* VehicleSubsystem, const FGuid& FromNodeGuid,
        const FGuid& ToNodeGuid, TSubclassOf<class AFGWheeledVehicle> VehicleType) const;
    /** Cost of the closed Producer/requester loop in the same units as the vanilla route-cost UI. */
    float EstimateLoopFuelConsumptionMW(class AFGWheeledVehicle* Vehicle, const TArray<FGuid>& Loop) const;
    /** Omits Fuel only when the live fuel inventory contains strictly more than two loop costs. */
    bool ShouldSkipFuelStop(AFGWheeledVehicleIdentifier* Vehicle, const TArray<FGuid>& LoopWithoutFuel) const;
    /** Selects and claims at most one open requester: the most starved stock/threshold ratio. */
    TArray<FGuid> SelectRequestersForResource(const FString& ResourceId, AFGWheeledVehicleIdentifier* Vehicle);
    /** Rebuilds the route at the producer boundary, preserving the remembered (F) position. */
    void ProcessDispatchTruckTarget(AFGWheeledVehicleIdentifier* Vehicle, const FString& ResourceId, const TCHAR* Trigger);
    /** Releases this truck's claim when the waypoint it just docked at is the claimed requester. */
    void ReleaseRequesterClaimIfDocked(AFGWheeledVehicleIdentifier* Vehicle);
    /** Drops claims whose truck disappeared or whose safety timeout elapsed. */
    void PruneStaleRequesterClaims();
    /** Drops every claim owned by a vehicle that no longer follows its dispatched route. */
    void ReleaseRequesterClaimsForVehicle(AFGWheeledVehicleIdentifier* Vehicle, const TCHAR* Reason);
    /** Resolves an exact localized display name to an item class. Cached after the first call. */
    TSubclassOf<class UFGItemDescriptor> ResolveItemClassByName(const FString& Name) const;
    /** Also accepts an arbitrary/language-specific id by inferring the sole cargo class at its LOAD producer. */
    TSubclassOf<class UFGItemDescriptor> ResolveItemClassForResource(const FString& ResourceId) const;
    /** Current stock at a dock's connected storage, in whole stacks (rounded down). */
    int32 ReadDockStockInStacks(AFGDockingStationIdentifier* Dock, TSubclassOf<class UFGItemDescriptor> ItemClass) const;
};
