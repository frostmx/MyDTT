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

    // The docking events themselves, from hooks on AFGWheeledVehicle::WasDocked_Implementation and
    // WasUndocked_Implementation (see MyDTTHooks). The identifier's docking flag is written inline
    // by those two, which is why the earlier hooks on its setter never fired. Game thread.
    void HandleTruckDocked(class AFGWheeledVehicle* Vehicle, class AFGBuildableDockingStation* Station);
    void HandleTruckUndocked(class AFGWheeledVehicle* Vehicle);
    /** A held (parked) truck was pushed out of its station by another actor docking there. */
    /** The DockActor hook dropped a hold on this station for another actor: a parked truck or a leftover drain lost its place. */
    void HandleHeldStationEvicted(class AFGBuildableDockingStation* Station, AActor* NewActor);
    const FDTTSettings& GetSettings() const { return Settings; }
    /** Sanitizes and applies; false (with a message) when nothing changed. Server only. */
    bool ApplySettings(const FDTTSettings& NewSettings, const TCHAR* Source, FString* OutMessage = nullptr);

    UFUNCTION(BlueprintCallable, Category = "Dynamic Time Table|Groups") FGuid CreateGroup(const FString& GroupName, EDTTGroupKind Kind = EDTTGroupKind::Train);
    UFUNCTION(BlueprintCallable, Category = "Dynamic Time Table|Groups") bool RemoveGroup(FGuid GroupId);
    UFUNCTION(BlueprintCallable, Category = "Dynamic Time Table|Groups") bool RenameGroup(FGuid GroupId, const FString& NewName);
    UFUNCTION(BlueprintCallable, Category = "Dynamic Time Table|Groups") bool AddStationByName(FGuid GroupId, const FString& StationName, int32 TargetSlots = 1);
    UFUNCTION(BlueprintCallable, Category = "Dynamic Time Table|Groups") bool RemoveStationByName(FGuid GroupId, const FString& StationName);
    UFUNCTION(BlueprintCallable, Category = "Dynamic Time Table|Groups") bool SetTargetSlots(FGuid GroupId, const FString& StationName, int32 TargetSlots);
    UFUNCTION(BlueprintCallable, Category = "Dynamic Time Table|Groups") void GetAllVanillaStationNames(TArray<FString>& OutStationNames) const;
    UFUNCTION(BlueprintCallable, Category = "Dynamic Time Table|Groups") void GetAllDockingStationNames(TArray<FString>& OutStationNames) const;
    UFUNCTION(BlueprintPure, Category = "Dynamic Time Table|Groups") const TArray<FDTTStationGroup>& GetStationGroups() const;
    /** Human-readable server-authoritative resource-dispatch diagnostics for /dtt dispatch.
     * ResourceFilter, when non-empty, limits the listing to stations tagged with that resource
     * (case-insensitive) and drops the fuel-stop section, since chat has no scrollback. */
    void GetDispatchStatusLines(TArray<FString>& OutLines, const FString& ResourceFilter = FString()) const;
    /** /dtt trucks: every dispatch and pool truck, one line each. */
    void GetTruckStatusLines(TArray<FString>& OutLines, const FString& ResourceFilter = FString()) const;
    /** Rows behind the Dispatch tab, the chat listing and the snapshot; depots and fuel stops only with no filter. */
    void BuildDispatchRows(TArray<FDTTDispatchRow>& OutRows, const FString& ResourceFilter = FString()) const;
    /** Rows behind the Trucks tab, the chat listing and the snapshot. */
    void BuildTruckRows(TArray<FDTTTruckRow>& OutRows, const FString& ResourceFilter = FString()) const;
    static FString FormatDispatchRow(const FDTTDispatchRow& Row);
    static FString FormatTruckRow(const FDTTTruckRow& Row);
    static FString PhaseLabel(EDTTTruckPhase Phase);
    UFUNCTION(BlueprintCallable, Category = "Dynamic Time Table|Groups") void GetGroupRuntimeLoad(FGuid GroupId, TArray<FDTTStationRuntimeLoad>& OutLoads) const;
    UFUNCTION(BlueprintCallable, Category = "Dynamic Time Table|Runtime") void RecalculateBookingsFromVanilla();
    int32 GetConfigurationRevision() const { return ConfigurationRevision; }
    /** Dispatch trucks' fuel anchors for ADTTSaveDataActor to write into the save. */
    void ExportTruckAnchors(TArray<FDTTSavedTruckAnchors>& OutAnchors) const;

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
     * UNLOAD a requester. A requester is open while its stock is below its threshold, and the
     * threshold is the station's capacity in stacks of that item: a station takes everything that
     * fits, so "not full" is the only demand it can express. "(Iron-10)" lowers that threshold by
     * ten stacks (open only with more than ten stacks free); N at or above the capacity never opens.
     * Legacy explicit "(Iron+)" remains supported. Parentheses deliberately differ from the
     * "[group,N]" balancing tag, so one station can carry both independently.
     */
    static bool ParseDispatchTag(const FString& FullName, FString& OutBaseName, FString& OutResourceId, EDTTDispatchRole& OutRole, int32& OutThreshold);
    /** The name without its dispatch tag, for display. */
    static FString StripDispatchTag(const FString& FullName);
    /** The inverse of ParseDispatchTag. */
    static FString MakeDispatchTaggedName(const FString& BaseName, const FString& ResourceId, EDTTDispatchRole Role, int32 Threshold);
    /** Exact, resource-independent marker for an optional dedicated refuelling waypoint. */
    static bool ParseFuelStopTag(const FString& FullName, FString& OutBaseName);
    /** Exact marker "(D)" for a depot: where a pool truck waits, held docked, between laps. */
    static bool ParseDepotTag(const FString& FullName, FString& OutBaseName);
    /** The name a player recognises: no group, dispatch, fuel or depot tag. */
    static FString StripAllTags(const FString& FullName);

    /** Pool trucks for ADTTSaveDataActor to write into the save. */
    void ExportPoolTrucks(TArray<FDTTSavedPoolTruck>& OutTrucks) const;
    /** /dtt depots: one line per depot with its claim, plus the pool trucks. */
    void GetDepotStatusLines(TArray<FString>& OutLines) const;
    /** /dtt park <truck>: sends a truck to the nearest free depot, joining the pool if needed. */
    bool CommandParkTruck(const FString& TruckQuery, FString& OutMessage);
    /** /dtt release <truck>: lets a parked truck go on a [Depot, Depot] loop; it parks again on return. */
    bool CommandReleaseTruck(const FString& TruckQuery, FString& OutMessage);

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
    // A pool truck between two laps whose target index is being reset. The autopilot zeroes an
    // index that points past the end of the route, but only on its own tick, so the two-stop
    // [Depot, Depot] written at the lap boundary must sit for a tick before the next lap is
    // written; the poll sees the index drop and drops the entry (or times it out).
    struct FDTTIndexReset
    {
        int32 IndexBefore = 0;
        double SinceSeconds = 0.0;
    };
    TMap<TWeakObjectPtr<AFGWheeledVehicleIdentifier>, FDTTIndexReset> IndexResets;
    /** Set by the poll when a reset has landed; the poll runs one dispatch pass for it afterwards. */
    bool bIndexResetDispatchDue = false;
    bool bTimeTableReevaluationScheduled = false;
    bool bRouteReevaluationScheduled = false;

    /** What the last poll saw for one vehicle. A change in either field is a signal to act on. */
    struct FDTTTruckPollState
    {
        int32 TargetIndex = INDEX_NONE;
        bool bDocking = false;
        // A one-stop route never changes its target index, so neither of the two signals above
        // ever fires for it; this paces the timed re-check that stands in for them.
        double LastIdleRecheckSeconds = 0.0;
        // The route as last seen, so that the game re-saving it unchanged (it does, on every
        // docking) is not mistaken for a player edit in HandleVehicleRouteChanged.
        TArray<FGuid> LastRoute;
        // What this mod wrote last, and what the route was right before that write. The game keeps
        // its own copy of the route across a docking and writes it back when the docking ends; a
        // lap rebuilt while the truck was docking is reverted by that. Seeing the pre-write route
        // come back is that revert, not a player edit, and the written route is put back.
        TArray<FGuid> LastWrittenRoute;
        TArray<FGuid> RouteBeforeLastWrite;
        // The station the docking hook reported, while the truck stands on it. The transfer plan
        // is keyed by it and cleared on undock.
        TWeakObjectPtr<class AFGBuildableDockingStation> DockedStation;
        // True once the hooks, not the poll, delivered this truck's docking edge.
        bool bDockedViaHook = false;
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
    // When a requester was first seen open with nobody claiming it, so the window can show how
    // long it has been waiting. Kept by the 1 Hz poll, not saved: a load restarts the clock.
    TMap<FGuid, double> RequesterOpenSinceWorldSeconds;
    /** Requesters already reported as DISPATCH_REQUEST_OVERDUE in the current waiting episode. */
    TSet<FGuid> OverdueRequestsLogged;

    // The tunables (FDTTSettings), saved by ADTTSaveDataActor since DataVersion 7.
    UPROPERTY()
    FDTTSettings Settings;

    // A managed truck held docked at the last requester of its lap while the station drains what
    // the threshold left on board: one entry per truck, torn down with the docking. Only with
    // LeftoverHoldSeconds > 0 (off by default since 2.4; the drain itself needs no hold). Runtime
    // only; after a load such a truck is simply undocked (HOLD_STALE).
    struct FDTTLeftoverHold
    {
        TWeakObjectPtr<class AFGBuildableDockingStation> Station;
        TSubclassOf<class UFGItemDescriptor> ItemClass;
        /** Set when the docking cycle's own transfer has run; the hold timer starts here. */
        bool bCycleDone = false;
        double StartedSeconds = 0.0;
        double LastTopUpSeconds = 0.0;
        int32 MovedByTopUps = 0;
    };
    TMap<TWeakObjectPtr<AFGWheeledVehicleIdentifier>, FDTTLeftoverHold> LeftoverHolds;

    // Fuel items on board when a truck docked at an (F) stop, compared on undocking: a pump that
    // gave nothing is the usual reason a truck strands, and it is invisible otherwise. Runtime only.
    TMap<TWeakObjectPtr<AFGWheeledVehicleIdentifier>, int32> FuelItemsAtDock;
    double LastDepotTopUpSeconds = 0.0;
    /** Once a second from the poll: top-ups, and the release on empty or timeout. */
    void TickLeftoverHolds(double Now);
    /** Once a second from the poll: a parked truck keeps unloading into a depot in UNLOAD mode. */
    void TickDepotDrains(double Now);
    /** Drops the hold; undocks when asked and the truck still stands there. Removes the entry. */
    void ReleaseLeftoverHold(AFGWheeledVehicleIdentifier* Vehicle, const TCHAR* Reason, bool bUndock);
    /** The stop is the last requester this lap planned: pool laps by State.Requesters, legacy laps by the anchors' PlannedItems in route order. */
    bool IsLastPlannedRequester(AFGWheeledVehicleIdentifier* Vehicle, const FGuid& StationGuid) const;
    /** The last UNLOAD requester of the resource in route order, whatever the lap planned. */
    bool IsLastRequesterInRoute(AFGWheeledVehicleIdentifier* Vehicle, const FGuid& StationGuid, const FString& ResourceId) const;
    /** Stacks of ItemClass on board; 0 when unreadable. */
    int32 OnBoardStacksOf(AFGWheeledVehicleIdentifier* Vehicle, TSubclassOf<class UFGItemDescriptor> ItemClass) const;
    /**
     * True while a parked truck's cargo is on its way into the depot: the depot is in UNLOAD mode
     * and depot unloading is on. The docking cycle drains it at its end, after the dispatch that
     * runs at parking, so that dispatch must not count it.
     */
    bool IsCargoDrainedAtDepot(const FDTTPoolTruckState& State) const;
    /** "Plastic x3, Screws x1" for what the truck carries. */
    FString DescribeCargo(AFGWheeledVehicleIdentifier* Vehicle, TSubclassOf<class UFGItemDescriptor>* OutSingleClass = nullptr) const;
    // Built once from UFGBlueprintFunctionLibrary::GetAllDescriptorsSorted, not per poll tick.
    mutable TMap<FString, TSubclassOf<class UFGItemDescriptor>> ItemClassByNameCache;
    // Producer is always Route[0]. A dedicated fuel anchor is optional and comes only from an
    // explicit (F) tag; it is remembered because a lap that does not need it leaves it out of the
    // live route. The one exception to "nothing here is SaveGame": ADTTSaveDataActor stores these,
    // since the route alone cannot bring an omitted (F) back after a load.
    TMap<TWeakObjectPtr<AFGWheeledVehicleIdentifier>, FDTTDispatchTruckAnchors> DispatchTruckAnchors;

    // Shared pool. Depots are the docks carrying "(D)", rebuilt with the other tags. A depot is
    // claimed by at most one truck: the one parked there or heading there. The pool state is the
    // second SaveGame exception, for the same reason as the anchors: a truck saved while parked
    // has to come back parked, and the route alone ([Depot, Depot]) does not say so.
    TSet<FGuid> DispatchDepotGuids;
    TMap<FGuid, TWeakObjectPtr<AFGWheeledVehicleIdentifier>> DepotClaims;
    TMap<TWeakObjectPtr<AFGWheeledVehicleIdentifier>, FDTTPoolTruckState> PoolTrucks;
    bool bParkedTrucksRestored = false;
    double LastPoolDispatchSeconds = 0.0;

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
    /**
     * Least-loaded station of the group, ties broken cyclically. Exclude leaves one truck's own
     * bookings out of the load: at a lap boundary its old route still holds last lap's (F), and
     * without this it would weigh against the very station it is about to choose from.
     */
    FDTTStationEntry* SelectBestStation(FDTTStationGroup& Group, const AFGWheeledVehicleIdentifier* Exclude = nullptr);
    int32 GetAssignedCount(AFGTrainStationIdentifier* Identifier) const;
    int32 GetEntryAssignedCount(const FDTTStationGroup& Group, const FDTTStationEntry& Entry, const AFGWheeledVehicleIdentifier* Exclude = nullptr) const;
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
    int32 GetDockAssignedCount(AFGDockingStationIdentifier* Identifier, const AFGWheeledVehicleIdentifier* Exclude = nullptr) const;
    int32 CountWaypointsInGroup(AFGWheeledVehicleIdentifier* Vehicle, const FDTTStationGroup& Group) const;
    bool ReplaceCurrentWaypoint(AFGWheeledVehicleIdentifier* Vehicle, int32 WaypointIndex, const FGuid& NewPathNodeGuid);
    /** Path network the group's vehicles travel, or INDEX_NONE when nothing in the group is moving yet. Diagnostics only. */
    int32 GetGroupPathNetworkID(const FDTTStationGroup& Group) const;

    // Resource dispatch. The lap limit (Settings.MaxRequestersPerLap), the fuel margins, the claim
    // timeout and the idle recheck interval live in FDTTSettings since 2.1; their stories stay here.
    // Settings.FuelReachFraction: ChooseFuelInsertIndex accepts a position only if the tank can
    // reach the (F) there using at most this fraction of its current energy - the per-segment
    // estimate is unverified, so a fraction of 1 plans a detour the truck may not survive.
    // Settings.FuelLegMarginFactor: the leg guard (EnsureFuelForNextLeg) demands this many times
    // the estimated cost of the next leg in the tank before letting the truck drive it; below that
    // a fuel stop goes in first. Both were constants until 2.5 for fear of the protocol; they are
    // the two numbers a world with long roads actually wants to move.
    // IsFuelCriticallyLow triggers this many estimated lap-costs early, not at exactly 1x: the
    // engine's per-segment fuel estimate is unverified against actual consumption, and a borrowed
    // stop lengthens the lap it is added to beyond what was checked - a bare 1x margin let a truck
    // reach exactly zero fuel mid-lap with no stop ever borrowed (see CHANGELOG 1.0.2). Raised from
    // 1.5 to 2.0 for the same reason, with more margin against that same estimate error.
    // ShouldSkipFuelStop only leaves a dedicated (F) out of a lap once the tank clears
    // FuelSkipMarginLaps estimated lap-costs - kept above FuelCriticalMarginLaps so there is a dead
    // zone between "borrow/warn" and "safe to skip" instead of the two thresholds meeting.
    // IdleDispatchRecheckSeconds: how often a dispatch truck parked on a one-stop [Producer] route
    // is re-evaluated; its target index never changes, so without this it would never notice the
    // first request opening.
    /** Resolves a neutral tag against the station's live LOAD/UNLOAD switch. */
    EDTTDispatchRole GetEffectiveDispatchRole(const FDTTDispatchStation& Station) const;
    /** LOAD is producer, UNLOAD is requester; a contradictory tag is never actionable. */
    bool DoesDispatchStationMatchMode(const FDTTDispatchStation& Station) const;
    /** Route[0] opts a truck into dispatch and supplies its resource when it is a live LOAD producer. */
    FString ResolveDispatchResourceFromProducer(const FGuid& ProducerGuid) const;
    /** Keeps the replicated vehicle name useful without using it as dispatch configuration. */
    void UpdateDispatchVehicleName(AFGWheeledVehicleIdentifier* Vehicle, const FString& ResourceId,
        const TArray<FGuid>& Route, int32 CurrentTargetIndex) const;
    /**
     * Producer is structurally Route[0]. An optional dedicated Fuel stop is selected only by its
     * explicit (F) station tag and remembered with its route position while a lap omits it.
     */
    bool UpdateRememberedAnchors(AFGWheeledVehicleIdentifier* Vehicle, const FString& ResourceId, const TArray<FGuid>& Route);
    /** Rewrites the complete route while suppressing this mod's SetVehicleRoute hook. */
    bool ReplaceRoute(AFGWheeledVehicleIdentifier* Vehicle, const TArray<FGuid>& NewRoute);
    /** Lap boundary: write [Depot, Depot] so the autopilot zeroes the target index; true when the dispatch must wait for the poll. */
    bool BeginIndexReset(AFGWheeledVehicleIdentifier* Vehicle, const struct FDTTPoolTruckState& State);
    /** Fuel cost for one directed edge in the vehicle path network, or a negative value on failure. */
    float SumSegmentFuelConsumption(class AFGVehicleSubsystem* VehicleSubsystem, const FGuid& FromNodeGuid,
        const FGuid& ToNodeGuid, TSubclassOf<class AFGWheeledVehicle> VehicleType) const;
    /** Fuel cost of the actual path between two arbitrary nodes on the same network, or negative on failure. */
    float EstimatePathFuelConsumptionMW(class AFGVehicleSubsystem* VehicleSubsystem, const class UFGVehiclePathPreset* Preset,
        TSubclassOf<class AFGWheeledVehicle> VehicleType, const FGuid& From, const FGuid& To) const;
    /** Cost of the closed Producer/requester loop in the same units as the vanilla route-cost UI. */
    float EstimateLoopFuelConsumptionMW(class AFGWheeledVehicle* Vehicle, const TArray<FGuid>& Loop) const;
    /** Current fuel stock and this loop's per-lap need, in fuel items. False if either is not estimable. */
    bool TryEstimateFuelMargin(AFGWheeledVehicleIdentifier* Vehicle, const TArray<FGuid>& LoopWithoutFuel,
        float& OutCurrentFuelItems, float& OutPerLapItems) const;
    /** Omits Fuel only when the live fuel inventory holds strictly more than FuelSkipMarginLaps loop costs. */
    bool ShouldSkipFuelStop(AFGWheeledVehicleIdentifier* Vehicle, const TArray<FGuid>& LoopWithoutFuel) const;
    /** Borrow trigger for a truck without its own (F): below FuelCriticalMarginLaps loop costs. */
    bool IsFuelCriticallyLow(AFGWheeledVehicleIdentifier* Vehicle, const TArray<FGuid>& LoopWithoutFuel) const;
    /** Nearest (F)-tagged dock reachable from FromGuid on the same path network, or invalid if none. */
    FGuid FindNearestFuelStop(class AFGWheeledVehicle* Vehicle, const FGuid& FromGuid) const;
    /**
     * The (F) a truck actually drives to: Seed itself when it stands alone, otherwise the
     * least-loaded member of Seed's [group] (SelectBestStation, the truck's own bookings left
     * out). One rule for an own (F), a borrowed one and a pool lap's.
     */
    FGuid ChooseFuelStopFromGroup(AFGWheeledVehicleIdentifier* Vehicle, const FGuid& Seed);
    /**
     * Mid-lap leg guard, run whenever a managed truck's target changes: the truck stands at the
     * stop before its target (StandingAt names it when the route cannot - a pool truck released
     * from its depot), and the tank must hold FuelLegMarginFactor times the estimated cost of the
     * leg ahead (plus the closing leg back to Route[0] when the target is the last stop of a
     * dispatch loop) and, on top of that, the detour from the target to the nearest (F) - so
     * that no leg is ever driven after which a pump is out of reach (measured: two short legs
     * of a lap each passed a 2x check and left a truck with 2.5 fuel items 2,300 MJ from home).
     * Otherwise the nearest reachable (F) from here - balanced within its group - is inserted in
     * front of the target. Index zero of a dispatch loop is the boundary, where the whole lap is
     * planned anew; a pool lap may be guarded at index zero from its depot. Returns true when the
     * route was rewritten; the inserted stop goes to OutEmergencyFuel, which the caller records
     * so the stop is neither adopted as the truck's own nor counted twice.
     */
    bool EnsureFuelForNextLeg(AFGWheeledVehicleIdentifier* Vehicle, bool bClosedLoop,
        FGuid& OutEmergencyFuel, const TCHAR* Trigger, const FGuid& StandingAt = FGuid());
    /** Tank energy against FuelLegMarginFactor times the estimated cost From -> To (plus ExtraCost). */
    bool TankCoversLeg(AFGWheeledVehicleIdentifier* Vehicle, const FGuid& From, const FGuid& To,
        float ExtraCost, float& OutNeed, float& OutEnergy) const;
    /**
     * A requester's levels, in stacks of the item. Trigger: the dock is called while its stock is
     * below it - the tag's N (solid: stacks; fluid: percent of the tank) or the settings' limit.
     * Target: a delivery fills the dock up to it - the trigger plus the settings' buffer, never
     * above the capacity. A trigger of 0 (tag "-0") never opens.
     */
    struct FDTTRequestLevels
    {
        int32 Capacity = 0;
        int32 Trigger = 0;
        int32 Target = 0;
        bool bFluid = false;
    };
    /** False when the dock's inventory cannot be read (neither stock nor capacity is known). */
    bool GetRequestLevels(const FDTTDispatchStation& Station, TSubclassOf<class UFGItemDescriptor> ItemClass, FDTTRequestLevels& Out) const;
    /** True for a truck whose cargo inventory carries liquids or gases. */
    bool IsFluidTruck(AFGWheeledVehicleIdentifier* Vehicle) const;
    /**
     * Truck cargo in the same "stacks" unit as ReadDockStockInStacks: capacity from the inventory's
     * slot size (a fluid truck's single slot is 64 stacks), on-board from what is loaded now.
     */
    bool EstimateTruckCargoStacks(AFGWheeledVehicleIdentifier* Vehicle, TSubclassOf<class UFGItemDescriptor> ItemClass,
        int32& OutCapacityStacks, int32& OutOnBoardStacks) const;
    /** One requester below its trigger, as CollectOpenRequesters reports it. */
    struct FDTTOpenRequester
    {
        const FDTTDispatchStation* Station = nullptr;
        int32 StockStacks = 0;
        int32 TriggerStacks = 0;
        int32 TargetStacks = 0;
        /** The dock's whole inventory in stacks: what a lap without exact quantities can leave here. */
        int32 CapacityStacks = 0;
        double FillRatio = 0.0;
    };
    /**
     * Every UNLOAD requester of the resource below its threshold that is not claimed by another
     * truck (ForVehicle's own claims count as free; null means any claim excludes), most starved
     * first. Pure: claims nothing.
     */
    void CollectOpenRequesters(const FString& ResourceId, TSubclassOf<class UFGItemDescriptor> ItemClass,
        AFGWheeledVehicleIdentifier* ForVehicle, TArray<FDTTOpenRequester>& Out) const;
    /**
     * Selects and claims the open requesters for one lap, most starved first, until their summed
     * deficit (threshold minus stock) covers what the truck can deliver, at most MaxRequestersPerLap.
     * The producer whose stock bounds the lap is Route[0] unless ProducerGuid says otherwise (a pool
     * truck sits at a depot, not at its producer); the planned items go to OutPlannedItems, or to
     * the legacy anchors when that is null.
     */
    TArray<FGuid> SelectRequestersForResource(const FString& ResourceId, AFGWheeledVehicleIdentifier* Vehicle,
        const FGuid& ProducerGuid = FGuid(), TMap<FGuid, int32>* OutPlannedItems = nullptr, int32* OutNeededStacks = nullptr,
        bool bIgnoreCargo = false);
    /**
     * The LOAD producer of the resource a pool truck should load at: the one with the most stock,
     * ties by path cost from FromGuid. Pure; the group rule (ChooseProducerFromGroup) is applied
     * by the caller once the truck is known.
     */
    FGuid ChooseProducerForResource(AFGWheeledVehicleIdentifier* Vehicle, const FString& ResourceId,
        TSubclassOf<class UFGItemDescriptor> ItemClass, const FGuid& FromGuid, int32& OutStockStacks) const;
    /**
     * The producer for the coming lap. With Route[0] in a [group], the group rule applied to the
     * producer side of a dispatch lap: the LOAD producer of the same resource in that group with
     * the fewest trucks per slot (ties: more stock, then the current one), and a booking on it
     * until the truck docks there. Without a group, CurrentProducer unchanged.
     */
    FGuid ChooseProducerFromGroup(AFGWheeledVehicleIdentifier* Vehicle, const FString& ResourceId, const FGuid& CurrentProducer);
    /**
     * Position in BaseLoop for a fuel stop: the insertion with the smallest detour among those the
     * tank can reach. FallbackIndex when no leg cost is estimable or no position is reachable.
     */
    int32 ChooseFuelInsertIndex(AFGWheeledVehicleIdentifier* Vehicle, const TArray<FGuid>& BaseLoop,
        const FGuid& FuelGuid, int32 FallbackIndex) const;
    /**
     * Mid-lap: heading for another requester with nothing left on board, the rest of the lap is
     * dropped so the truck returns to the producer. Returns true when the route was rewritten.
     */
    bool CutLapIfCargoEmpty(AFGWheeledVehicleIdentifier* Vehicle, const FString& ResourceId,
        const TArray<FGuid>& Route, int32 Index, const TCHAR* Trigger, const FGuid& KeepTail = FGuid());
    /**
     * Rebuilds the whole lap at the producer boundary: Producer, the claimed requesters (or, with no
     * demand, the last consumer), and an (F) only when this lap's fuel needs it. The one mid-lap
     * action is CutLapIfCargoEmpty.
     */
    void ProcessDispatchTruckTarget(AFGWheeledVehicleIdentifier* Vehicle, const FString& ResourceId, const TCHAR* Trigger);
    /** Releases this truck's claim when the waypoint it just docked at is the claimed requester. */
    /**
     * Releases this truck's claim on the dock it just docked at: DockGuid when the docking hook
     * supplied the station, otherwise the waypoint before the current target - at docking the
     * autopilot has already moved the target on to the next stop (measured), so the current target
     * is the stop the truck is heading for next, not the one it is standing at.
     */
    void ReleaseRequesterClaimIfDocked(AFGWheeledVehicleIdentifier* Vehicle, const FGuid& DockGuid = FGuid());
    /** Drops claims whose truck disappeared or whose safety timeout elapsed. */
    void PruneStaleRequesterClaims();
    /** Open (below its trigger, in UNLOAD) as the window and the dispatcher both define it. */
    bool IsRequesterOpen(const FDTTDispatchStation& Station, TSubclassOf<class UFGItemDescriptor> ItemClass) const;
    /** Starts or stops the waiting clock of every requester; logs the first crossing of OverdueRequestMinutes. */
    void UpdateRequesterWaitTimes();
    /** Drops every claim owned by a vehicle that no longer follows its dispatched route. */
    void ReleaseRequesterClaimsForVehicle(AFGWheeledVehicleIdentifier* Vehicle, const TCHAR* Reason);
    /** Resolves an exact localized display name to an item class. Cached after the first call. */
    TSubclassOf<class UFGItemDescriptor> ResolveItemClassByName(const FString& Name) const;
    /** Also accepts an arbitrary/language-specific id by inferring the sole cargo class at its LOAD producer. */
    TSubclassOf<class UFGItemDescriptor> ResolveItemClassForResource(const FString& ResourceId) const;
    /** Current stock at a dock's connected storage, in whole stacks (rounded down). */
    int32 ReadDockStockInStacks(AFGDockingStationIdentifier* Dock, TSubclassOf<class UFGItemDescriptor> ItemClass) const;
    /**
     * What the dock's storage holds when full, in the same stacks unit; false (not 0) when the dock,
     * its building or its inventory cannot be read. Same formula as EstimateTruckCargoStacks: slot
     * size, not stack size, so a fluid station's single 3 200 000 L slot counts as 64 stacks.
     */
    bool ReadDockCapacityInStacks(AFGDockingStationIdentifier* Dock, TSubclassOf<class UFGItemDescriptor> ItemClass, int32& OutCapacityStacks) const;
    const FDTTDispatchStation* FindDispatchStationByGuid(const FGuid& PathNodeGuid) const;
    /**
     * What the station may move to or from this truck at the end of its docking cycle, applied
     * by the transfer hook (see MyDTTHooks). For a dispatch truck under exact quantities: the
     * producer fills it up to this lap's summed requester deficits, a requester of the lap's
     * resource is filled up to its threshold, the last one drains, any other stop moves what the
     * game would. False for a truck this mod does not manage and, apart from a depot, whenever
     * exact quantities are off - the game's own transfer runs untouched then. bOutLeftoverHold is
     * set only when the drain is to be followed by a hold (LeftoverHoldSeconds > 0).
     */
    bool ComputeTransferPlan(AFGWheeledVehicleIdentifier* Vehicle, class AFGBuildableDockingStation* Station, struct FDTTTransferPlan& OutPlan,
        bool* bOutLeftoverHold = nullptr) const;

    // Shared pool.
    bool IsDepotGuid(const FGuid& PathNodeGuid) const { return DispatchDepotGuids.Contains(PathNodeGuid); }
    bool RouteHasDepot(const TArray<FGuid>& Route) const;
    /** The pool entry, created (POOL_JOINED, ToDepot) when the route carries a depot; null otherwise. */
    FDTTPoolTruckState* EnsurePoolMembership(AFGWheeledVehicleIdentifier* Vehicle, const TArray<FGuid>& Route, const TCHAR* Reason);
    /** Creates (or resets) the pool entry as ToDepot, claiming DepotGuid if valid, and drops the legacy bookkeeping. */
    FDTTPoolTruckState& JoinPool(AFGWheeledVehicleIdentifier* Vehicle, const FGuid& DepotGuid, const TCHAR* Reason);
    void LeavePool(AFGWheeledVehicleIdentifier* Vehicle, const TCHAR* Reason);
    /** Nearest unclaimed depot reachable from FromGuid (a depot this truck already claims counts as free). */
    FGuid FindNearestDepot(AFGWheeledVehicleIdentifier* Vehicle, const FGuid& FromGuid) const;
    /** A fluid truck cannot unload at a solid depot and the other way round: the game never runs the transfer. Unknown station: true. */
    bool DepotFitsTruck(const FGuid& DepotGuid, AFGWheeledVehicleIdentifier* Vehicle) const;
    void ClaimDepot(const FGuid& DepotGuid, AFGWheeledVehicleIdentifier* Vehicle);
    void ReleaseDepotClaim(AFGWheeledVehicleIdentifier* Vehicle, const TCHAR* Reason);
    /** Holds the truck at the depot it just docked at. */
    void ParkTruck(AFGWheeledVehicleIdentifier* Vehicle, FDTTPoolTruckState& State, class AFGBuildableDockingStation* Station);
    /** Writes NewRoute, drops the hold and undocks the parked truck, which then drives to NewRoute[0]. */
    bool ReleaseParkedTruck(AFGWheeledVehicleIdentifier* Vehicle, FDTTPoolTruckState& State, const TArray<FGuid>& NewRoute, EDTTTruckPhase NewPhase, const TCHAR* Reason);
    /** Drops the hold without a route change; the truck stays where it is (player took over, eviction, lost). */
    void UnparkTruck(AFGWheeledVehicleIdentifier* Vehicle, FDTTPoolTruckState& State, EDTTTruckPhase NewPhase, const TCHAR* Reason, bool bUndock);
    /** The pool truck's share of ProcessCurrentTruckTarget. */
    void ProcessPoolTruckTarget(AFGWheeledVehicleIdentifier* Vehicle, FDTTPoolTruckState& State, const TCHAR* Trigger);
    /** Matches open requests to parked trucks. */
    void RunPoolDispatch(const TCHAR* Trigger);
    /** Writes the lap [Producer, Requesters..., (F)?, Depot] for one parked truck and lets it go. */
    bool AssignPoolTruck(AFGWheeledVehicleIdentifier* Vehicle, FDTTPoolTruckState& State, const FString& ResourceId,
        TSubclassOf<class UFGItemDescriptor> ItemClass, const FGuid& ProducerGuid, const TCHAR* Trigger);
    /** After a load: re-holds the trucks saved as parked, or sends stale ones back to a depot. */
    void RestoreParkedTrucks();
    void UpdatePoolVehicleName(AFGWheeledVehicleIdentifier* Vehicle, const FDTTPoolTruckState& State) const;
    /** Display name of a dock without any of the tags, for names and chat lines. */
    FString DockDisplayName(const FGuid& PathNodeGuid) const;
    /** A truck by its log name ("Name#1234"), its object number, or its display name. */
    AFGWheeledVehicleIdentifier* FindVehicleByQuery(const FString& Query) const;
};
