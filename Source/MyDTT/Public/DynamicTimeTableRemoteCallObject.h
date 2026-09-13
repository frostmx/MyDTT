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
    };
}
USTRUCT(BlueprintType)
struct FDTTClientSnapshot
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) int32 ProtocolVersion = 8;
    UPROPERTY(BlueprintReadOnly) int32 Revision = 0;
    // EDTTSnapshotSection bits this snapshot carries; the client keeps the other sections it has.
    UPROPERTY(BlueprintReadOnly) int32 Sections = 0;
    UPROPERTY(BlueprintReadOnly) TArray<FDTTStationGroup> Groups;
    UPROPERTY(BlueprintReadOnly) TArray<FString> AvailableStations;
    UPROPERTY(BlueprintReadOnly) TArray<FString> AvailableTruckStations;
    UPROPERTY(BlueprintReadOnly) TArray<FDTTStationRuntimeLoad> RuntimeLoads;
    UPROPERTY(BlueprintReadOnly) TArray<FDTTDispatchRow> DispatchRows;
    UPROPERTY(BlueprintReadOnly) TArray<FDTTTruckRow> TruckRows;
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
    static constexpr int32 ProtocolVersion = 8;
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
    UFUNCTION(Client, Reliable) void Client_ReceiveSnapshot(const FDTTClientSnapshot& Snapshot);
    UFUNCTION(Client, Reliable) void Client_OperationResult(bool bSuccess, const FString& Message);
    bool Check(int32 ClientProtocol);
    void SendSnapshot(int32 Sections);
    void StartLiveUpdates();
    void PollLiveSnapshot();

    FTimerHandle LiveUpdateTimerHandle;
};
