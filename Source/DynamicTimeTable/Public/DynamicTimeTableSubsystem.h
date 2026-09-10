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
class DYNAMICTIMETABLE_API UDynamicTimeTableSubsystem : public UWorldSubsystem
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
    UFUNCTION(BlueprintCallable, Category = "Dynamic Time Table|Groups") void GetGroupRuntimeLoad(FGuid GroupId, TArray<FDTTStationRuntimeLoad>& OutLoads) const;
    UFUNCTION(BlueprintCallable, Category = "Dynamic Time Table|Runtime") void RecalculateBookingsFromVanilla();
    int32 GetConfigurationRevision() const { return ConfigurationRevision; }

    UPROPERTY(BlueprintAssignable, Category = "Dynamic Time Table")
    FDTTGroupsChangedDelegate OnGroupsChanged;

    FDTTGroupsChangedNativeDelegate OnGroupsChangedNative;

private:
    TArray<FDTTStationGroup> StationGroups;
    TMap<AFGTrain*, AFGTrainStationIdentifier*> ActiveBookings;
    TMap<AFGWheeledVehicleIdentifier*, AFGDockingStationIdentifier*> TruckBookings;
    TWeakObjectPtr<ADTTSaveDataActor> SaveDataActor;
    bool bPersistenceReady = false;
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

    void InitializePersistence();
    void RegisterDTTChatCommand();
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
    bool ResolveStationReferences();
    bool RefreshStationDisplayName(AFGTrainStationIdentifier* Identifier);
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
    bool RefreshDockDisplayName(AFGDockingStationIdentifier* Identifier);
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
};
