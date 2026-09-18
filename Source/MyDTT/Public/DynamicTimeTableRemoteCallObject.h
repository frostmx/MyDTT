#pragma once
#include "CoreMinimal.h"
#include "FGRemoteCallObject.h"
#include "DynamicTimeTableTypes.h"
#include "DynamicTimeTableRemoteCallObject.generated.h"
/** Which parts of the snapshot a request asks for; the window asks only for its open tab. */
namespace EDTTSnapshotSection
{
    enum Type : int32
    {
        None = 0,
        Groups = 1,
        Dispatch = 2,
        Trucks = 4,
        Stations = 8,
    };
}
USTRUCT(BlueprintType)
struct FDTTClientSnapshot
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) int32 ProtocolVersion = 0;
    UPROPERTY(BlueprintReadOnly) int32 Revision = 0;
    // EDTTSnapshotSection bits this snapshot carries; the client keeps the other sections it has.
    UPROPERTY(BlueprintReadOnly) int32 Sections = 0;
    UPROPERTY(BlueprintReadOnly) TArray<FDTTStationGroup> Groups;
    UPROPERTY(BlueprintReadOnly) TArray<FString> AvailableStations;
    UPROPERTY(BlueprintReadOnly) TArray<FString> AvailableTruckStations;
    UPROPERTY(BlueprintReadOnly) TArray<FDTTStationRuntimeLoad> RuntimeLoads;
    UPROPERTY(BlueprintReadOnly) TArray<FDTTDispatchRow> DispatchRows;
    UPROPERTY(BlueprintReadOnly) TArray<FDTTTruckRow> TruckRows;
    UPROPERTY(BlueprintReadOnly) TArray<FDTTStationRow> StationRows;
    /**
     * Every station and truck name the rows above refer to, each written once. A truck's route
     * stops used to ride as full names on every truck: 400 trucks with six stops each is about
     * 105 KB of mostly the same dozen station names, against a 64 KB reliable bunch that the
     * server drops in silence. The rows carry places in these tables instead, and the client
     * puts the names back in Client_ReceiveSnapshot, so nothing downstream sees the difference.
     */
    UPROPERTY(BlueprintReadOnly) TArray<FString> DockNames;
    UPROPERTY(BlueprintReadOnly) TArray<FString> TruckNames;
    // Sent with the Groups section: small, and it changes with the configuration revision.
    UPROPERTY(BlueprintReadOnly) FDTTSettings Settings;
};
DECLARE_MULTICAST_DELEGATE(FDTTClientSnapshotChanged);
DECLARE_MULTICAST_DELEGATE_TwoParams(FDTTClientOperationResult, bool, const FString&);
UCLASS()
class MYDTT_API UDynamicTimeTableRemoteCallObject : public UFGRemoteCallObject
{
    GENERATED_BODY()
public:
    // 4: groups come from tags in the station names, and every edit is a rename.
    // 5: sectioned snapshots with the dispatch and truck rows.
    // 6: the settings ride with the Groups section and can be set from the window.
    // 7: request limits and buffer in the settings, dispatch rows carry trigger and target.
    // 8: dispatch and truck rows carry the item class, so the window can draw the icons.
    // 9: dispatch rows carry the waiting time, overdue minutes in the settings.
    // 10: the exact-quantities switch in the settings.
    // 11: stay-until-empty and the two fuel margins in the settings.
    // 12: the auto-name switch and its four tunables in the settings.
    // 13: the naming language in the settings.
    // 14: a station's "=" marker can be set from the station's own window.
    // 15: rows carry their dock or truck and a place in the snapshot's name tables, and the
    //     window can park, release, rebuild a lap and switch an autopilot.
    // 16: the Stations section and its table of docks, and the window can set a station's limit
    //     (one dock or many at once), its "=" marker, drop a request's claim and ask the
    //     dispatcher to look again.
    static constexpr int32 ProtocolVersion = 16;
    // A section is cut here so that the whole snapshot stays well under the 64 KB reliable bunch.
    static constexpr int32 MaxSnapshotRows = 400;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
    const FDTTClientSnapshot& GetSnapshot() const { return CachedSnapshot; }
    FDTTClientSnapshotChanged OnSnapshotChanged;
    FDTTClientOperationResult OnOperationResult;
    void RequestSnapshot(int32 Sections = EDTTSnapshotSection::Groups);
    void RequestCreateGroup(const FString& GroupName, EDTTGroupKind Kind);
    void RequestRenameGroup(FGuid GroupId, const FString& NewName);
    void RequestDeleteGroup(FGuid GroupId);
    void RequestAddStation(FGuid GroupId, const FString& StationName);
    void RequestRemoveStation(FGuid GroupId, const FString& StationName);
    void RequestSetTargetSlots(FGuid GroupId, const FString& StationName, int32 TargetSlots);
    void RequestRecalculate();
    void RequestSetSettings(const FDTTSettings& Settings);
    /**
     * The "=" marker of one docking station, from the button this mod puts in that station's own
     * window. The station is passed as the actor: the player is standing in front of it, so it is
     * replicated to them. Nothing comes back but the usual operation result - the marker lives in
     * the station's name, which the game replicates by itself, so the client reads it from there.
     */
    void RequestSetStationStay(class AFGBuildableDockingStation* Station, bool bOn);
    /**
     * The window's truck buttons. A truck is named by its identifier actor, which the game
     * replicates to every client at all times - the vehicle itself only comes to a client
     * standing near it, so it is no use for a table of the whole map. What comes back is the
     * usual result plus the Trucks section, so the row that changed redraws at once instead of
     * staying stale until the next poll.
     */
    void RequestParkTruck(class AFGWheeledVehicleIdentifier* Truck);
    void RequestReleaseTruck(class AFGWheeledVehicleIdentifier* Truck);
    void RequestSetAutopilot(class AFGWheeledVehicleIdentifier* Truck, bool bEnabled);
    void RequestRebuildLap(class AFGWheeledVehicleIdentifier* Truck);
    /**
     * The Stations tab's controls. A dock is named by its identifier actor for the same reason a
     * truck is; what comes back is the result plus the Stations section. The many-form is one
     * call on purpose: the server renames every dock and rebuilds the groups once, which a row
     * at a time could not do.
     */
    void RequestSetDockTrigger(class AFGDockingStationIdentifier* Dock, int32 Threshold);
    void RequestSetDockTriggerMany(const TArray<class AFGDockingStationIdentifier*>& Docks, int32 Threshold);
    void RequestSetDockStay(class AFGDockingStationIdentifier* Dock, bool bOn);
    void RequestClearDockClaim(FGuid PathNodeGuid);
    void RequestSendTruckNow(class AFGDockingStationIdentifier* Dock);
    UFUNCTION(Client, Reliable)
    void Client_OpenWindow();
private:
    UPROPERTY(Replicated) bool bReplicationAnchor = true;
    UPROPERTY() FDTTClientSnapshot CachedSnapshot;
    UFUNCTION(Server, Reliable) void Server_RequestSnapshot(int32 ClientProtocol, int32 Sections);
    UFUNCTION(Server, Reliable) void Server_CreateGroup(const FString& GroupName, EDTTGroupKind Kind, int32 ClientProtocol);
    UFUNCTION(Server, Reliable) void Server_RenameGroup(FGuid GroupId, const FString& NewName, int32 ClientProtocol);
    UFUNCTION(Server, Reliable) void Server_DeleteGroup(FGuid GroupId, int32 ClientProtocol);
    UFUNCTION(Server, Reliable) void Server_AddStation(FGuid GroupId, const FString& StationName, int32 ClientProtocol);
    UFUNCTION(Server, Reliable) void Server_RemoveStation(FGuid GroupId, const FString& StationName, int32 ClientProtocol);
    UFUNCTION(Server, Reliable) void Server_SetTargetSlots(FGuid GroupId, const FString& StationName, int32 TargetSlots, int32 ClientProtocol);
    UFUNCTION(Server, Reliable) void Server_Recalculate(int32 ClientProtocol);
    UFUNCTION(Server, Reliable) void Server_SetSettings(const FDTTSettings& Settings, int32 ClientProtocol);
    UFUNCTION(Server, Reliable) void Server_SetStationStay(class AFGBuildableDockingStation* Station, bool bOn, int32 ClientProtocol);
    UFUNCTION(Server, Reliable) void Server_ParkTruck(class AFGWheeledVehicleIdentifier* Truck, int32 ClientProtocol);
    UFUNCTION(Server, Reliable) void Server_ReleaseTruck(class AFGWheeledVehicleIdentifier* Truck, int32 ClientProtocol);
    UFUNCTION(Server, Reliable) void Server_SetAutopilot(class AFGWheeledVehicleIdentifier* Truck, bool bEnabled, int32 ClientProtocol);
    UFUNCTION(Server, Reliable) void Server_RebuildLap(class AFGWheeledVehicleIdentifier* Truck, int32 ClientProtocol);
    UFUNCTION(Server, Reliable) void Server_SetDockTrigger(const TArray<class AFGDockingStationIdentifier*>& Docks, int32 Threshold, int32 ClientProtocol);
    UFUNCTION(Server, Reliable) void Server_SetDockStay(class AFGDockingStationIdentifier* Dock, bool bOn, int32 ClientProtocol);
    UFUNCTION(Server, Reliable) void Server_ClearDockClaim(FGuid PathNodeGuid, int32 ClientProtocol);
    UFUNCTION(Server, Reliable) void Server_SendTruckNow(class AFGDockingStationIdentifier* Dock, int32 ClientProtocol);
    UFUNCTION(Client, Reliable) void Client_ReceiveSnapshot(const FDTTClientSnapshot& Snapshot);
    UFUNCTION(Client, Reliable) void Client_OperationResult(bool bSuccess, const FString& Message);
    bool Check(int32 ClientProtocol);
    /** The tail every truck operation shares: the result, then the rows the player is looking at. */
    void FinishTruckOperation(bool bSuccess, const FString& Message);
    /** The same for a station operation, with the Stations rows instead. */
    void FinishStationOperation(bool bSuccess, const FString& Message);
    void SendSnapshot(int32 Sections);
    void StartLiveUpdates();
    void PollLiveSnapshot();

    FTimerHandle LiveUpdateTimerHandle;
};
