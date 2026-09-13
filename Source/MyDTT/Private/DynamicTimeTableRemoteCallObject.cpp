#include "DynamicTimeTableRemoteCallObject.h"
#include "DynamicTimeTableSubsystem.h"
#include "DynamicTimeTableWidget.h"
#include "FGPlayerController.h"
#include "Net/UnrealNetwork.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintLibrary.h"
#include "TimerManager.h"
DEFINE_LOG_CATEGORY_STATIC(LogDynamicTimeTableRCO, Log, All);
void UDynamicTimeTableRemoteCallObject::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps)const{Super::GetLifetimeReplicatedProps(OutLifetimeProps);DOREPLIFETIME(UDynamicTimeTableRemoteCallObject,bReplicationAnchor);}
bool UDynamicTimeTableRemoteCallObject::Check(int32 V){if(V==ProtocolVersion)return true;Client_OperationResult(false,TEXT("dTT client/server protocol mismatch."));return false;}
void UDynamicTimeTableRemoteCallObject::SendSnapshot(int32 Sections)
{
    FDTTClientSnapshot X;
    X.ProtocolVersion = ProtocolVersion;
    X.Sections = Sections == EDTTSnapshotSection::None ? EDTTSnapshotSection::Groups : Sections;
    auto* S = GetWorld()->GetSubsystem<UDynamicTimeTableSubsystem>();
    if (S)
    {
        X.Revision = S->GetConfigurationRevision();
        if (X.Sections & EDTTSnapshotSection::Groups)
        {
            X.Groups = S->GetStationGroups();
            S->GetAllVanillaStationNames(X.AvailableStations);
            S->GetAllDockingStationNames(X.AvailableTruckStations);
            for (const auto& G : X.Groups) { TArray<FDTTStationRuntimeLoad> L; S->GetGroupRuntimeLoad(G.GroupId, L); X.RuntimeLoads.Append(L); }
            X.Settings = S->GetSettings();
        }
        if (X.Sections & EDTTSnapshotSection::Dispatch)
        {
            S->BuildDispatchRows(X.DispatchRows);
            if (X.DispatchRows.Num() > MaxSnapshotRows)
            {
                UE_LOG(LogDynamicTimeTableRCO, Warning, TEXT("SNAPSHOT_TRUNCATED Section=Dispatch Rows=%d Max=%d"), X.DispatchRows.Num(), MaxSnapshotRows);
                X.DispatchRows.SetNum(MaxSnapshotRows);
            }
        }
        if (X.Sections & EDTTSnapshotSection::Trucks)
        {
            S->BuildTruckRows(X.TruckRows);
            if (X.TruckRows.Num() > MaxSnapshotRows)
            {
                UE_LOG(LogDynamicTimeTableRCO, Warning, TEXT("SNAPSHOT_TRUNCATED Section=Trucks Rows=%d Max=%d"), X.TruckRows.Num(), MaxSnapshotRows);
                X.TruckRows.SetNum(MaxSnapshotRows);
            }
        }
    }
    // Rough size, for the log: strings dominate, everything else is a few bytes per row.
    int64 Bytes = 64;
    for (const auto& G : X.Groups) { Bytes += 48 + G.GroupName.Len(); for (const auto& E : G.Stations) Bytes += 32 + E.DisplayName.Len(); }
    for (const auto& N : X.AvailableStations) Bytes += 4 + N.Len();
    for (const auto& N : X.AvailableTruckStations) Bytes += 4 + N.Len();
    for (const auto& L : X.RuntimeLoads) Bytes += 20 + L.StationName.Len();
    for (const auto& R : X.DispatchRows) Bytes += 48 + R.ResourceId.Len() + R.StationName.Len() + R.ClaimOwner.Len() + R.Note.Len();
    for (const auto& R : X.TruckRows) { Bytes += 40 + R.Name.Len() + R.ResourceId.Len() + R.DepotName.Len() + R.PlannedSummary.Len(); for (const auto& Stop : R.RouteStops) Bytes += 4 + Stop.Len(); }
    UE_LOG(LogDynamicTimeTableRCO, Verbose, TEXT("SNAPSHOT_SIZE Sections=%d Groups=%d DispatchRows=%d TruckRows=%d ApproxBytes=%lld"),
        X.Sections, X.Groups.Num(), X.DispatchRows.Num(), X.TruckRows.Num(), Bytes);
    Client_ReceiveSnapshot(X);
}
void UDynamicTimeTableRemoteCallObject::RequestSnapshot(int32 Sections){Server_RequestSnapshot(ProtocolVersion,Sections);}void UDynamicTimeTableRemoteCallObject::RequestCreateGroup(const FString&N,EDTTGroupKind K){Server_CreateGroup(N,K,ProtocolVersion);}void UDynamicTimeTableRemoteCallObject::RequestRenameGroup(FGuid G,const FString&N){Server_RenameGroup(G,N,ProtocolVersion);}void UDynamicTimeTableRemoteCallObject::RequestDeleteGroup(FGuid G){Server_DeleteGroup(G,ProtocolVersion);}void UDynamicTimeTableRemoteCallObject::RequestAddStation(FGuid G,const FString&N){Server_AddStation(G,N,ProtocolVersion);}void UDynamicTimeTableRemoteCallObject::RequestRemoveStation(FGuid G,const FString&N){Server_RemoveStation(G,N,ProtocolVersion);}void UDynamicTimeTableRemoteCallObject::RequestSetTargetSlots(FGuid G,const FString&N,int32 V){Server_SetTargetSlots(G,N,FMath::Clamp(V,1,999),ProtocolVersion);}void UDynamicTimeTableRemoteCallObject::RequestRecalculate(){Server_Recalculate(ProtocolVersion);}
void UDynamicTimeTableRemoteCallObject::RequestSetSettings(const FDTTSettings& Settings){Server_SetSettings(Settings,ProtocolVersion);}
void UDynamicTimeTableRemoteCallObject::Server_SetSettings_Implementation(const FDTTSettings& Settings,int32 V){if(!Check(V))return;auto*S=GetWorld()->GetSubsystem<UDynamicTimeTableSubsystem>();FString M(TEXT("dTT: subsystem unavailable."));const bool O=S&&S->ApplySettings(Settings,TEXT("remote client"),&M);Client_OperationResult(O,M);SendSnapshot(EDTTSnapshotSection::Groups);}
void UDynamicTimeTableRemoteCallObject::Server_RequestSnapshot_Implementation(int32 V,int32 Sections){if(Check(V))SendSnapshot(Sections);}
void UDynamicTimeTableRemoteCallObject::Server_CreateGroup_Implementation(const FString&N,EDTTGroupKind K,int32 V){if(!Check(V))return;auto*S=GetWorld()->GetSubsystem<UDynamicTimeTableSubsystem>();bool O=S&&S->CreateGroup(N.TrimStartAndEnd().Left(64),K).IsValid();Client_OperationResult(O,O?TEXT("Group created."):TEXT("Create failed."));SendSnapshot(EDTTSnapshotSection::Groups);}
void UDynamicTimeTableRemoteCallObject::Server_RenameGroup_Implementation(FGuid G,const FString&N,int32 V){if(!Check(V))return;auto*S=GetWorld()->GetSubsystem<UDynamicTimeTableSubsystem>();bool O=S&&S->RenameGroup(G,N.TrimStartAndEnd().Left(64));Client_OperationResult(O,O?TEXT("Group renamed."):TEXT("Rename failed."));SendSnapshot(EDTTSnapshotSection::Groups);}
void UDynamicTimeTableRemoteCallObject::Server_DeleteGroup_Implementation(FGuid G,int32 V){if(!Check(V))return;auto*S=GetWorld()->GetSubsystem<UDynamicTimeTableSubsystem>();bool O=S&&S->RemoveGroup(G);Client_OperationResult(O,O?TEXT("Group deleted."):TEXT("Delete failed."));SendSnapshot(EDTTSnapshotSection::Groups);}
void UDynamicTimeTableRemoteCallObject::Server_AddStation_Implementation(FGuid G,const FString&N,int32 V){if(!Check(V))return;auto*S=GetWorld()->GetSubsystem<UDynamicTimeTableSubsystem>();bool O=S&&S->AddStationByName(G,N,1);Client_OperationResult(O,O?TEXT("Station added."):TEXT("Add failed."));SendSnapshot(EDTTSnapshotSection::Groups);}
void UDynamicTimeTableRemoteCallObject::Server_RemoveStation_Implementation(FGuid G,const FString&N,int32 V){if(!Check(V))return;auto*S=GetWorld()->GetSubsystem<UDynamicTimeTableSubsystem>();bool O=S&&S->RemoveStationByName(G,N);Client_OperationResult(O,O?TEXT("Station removed."):TEXT("Remove failed."));SendSnapshot(EDTTSnapshotSection::Groups);}
void UDynamicTimeTableRemoteCallObject::Server_SetTargetSlots_Implementation(FGuid G,const FString&N,int32 Slots,int32 V){if(!Check(V))return;auto*S=GetWorld()->GetSubsystem<UDynamicTimeTableSubsystem>();bool O=S&&S->SetTargetSlots(G,N,FMath::Clamp(Slots,1,999));Client_OperationResult(O,O?TEXT("Slots updated."):TEXT("Update failed."));SendSnapshot(EDTTSnapshotSection::Groups);}
void UDynamicTimeTableRemoteCallObject::Server_Recalculate_Implementation(int32 V){if(!Check(V))return;if(auto*S=GetWorld()->GetSubsystem<UDynamicTimeTableSubsystem>())S->RecalculateBookingsFromVanilla();Client_OperationResult(true,TEXT("Bookings recalculated."));SendSnapshot(EDTTSnapshotSection::Groups);}
void UDynamicTimeTableRemoteCallObject::Client_ReceiveSnapshot_Implementation(const FDTTClientSnapshot&X)
{
    // Only the sections this snapshot carries are replaced; the tab that is not open keeps what
    // it last saw.
    CachedSnapshot.ProtocolVersion = X.ProtocolVersion;
    CachedSnapshot.Revision = X.Revision;
    CachedSnapshot.Sections |= X.Sections;
    if (X.Sections & EDTTSnapshotSection::Groups)
    {
        CachedSnapshot.Groups = X.Groups;
        CachedSnapshot.AvailableStations = X.AvailableStations;
        CachedSnapshot.AvailableTruckStations = X.AvailableTruckStations;
        CachedSnapshot.RuntimeLoads = X.RuntimeLoads;
        CachedSnapshot.Settings = X.Settings;
    }
    if (X.Sections & EDTTSnapshotSection::Dispatch) CachedSnapshot.DispatchRows = X.DispatchRows;
    if (X.Sections & EDTTSnapshotSection::Trucks) CachedSnapshot.TruckRows = X.TruckRows;
    OnSnapshotChanged.Broadcast();
}
void UDynamicTimeTableRemoteCallObject::Client_OperationResult_Implementation(bool O,const FString&M){OnOperationResult.Broadcast(O,M);}
void UDynamicTimeTableRemoteCallObject::Client_OpenWindow_Implementation()
{
    AFGPlayerController* PC = GetTypedOuter<AFGPlayerController>();
    if (!IsValid(PC) || !PC->IsLocalController()) return;

    TArray<UUserWidget*> Widgets;
    UWidgetBlueprintLibrary::GetAllWidgetsOfClass(
        PC, Widgets, UDynamicTimeTableWidget::StaticClass(), true);

    for (UUserWidget* Widget : Widgets)
    {
        if (IsValid(Widget))
        {
            Widget->RemoveFromParent();
            if (UWorld* World = GetWorld())
            {
                World->GetTimerManager().ClearTimer(LiveUpdateTimerHandle);
            }
            return;
        }
    }

    UDynamicTimeTableWidget* Window = CreateWidget<UDynamicTimeTableWidget>(
        PC, UDynamicTimeTableWidget::StaticClass());
    if (!IsValid(Window)) return;

    Window->AddToViewport(1000);
    FInputModeGameAndUI Mode;
    Mode.SetWidgetToFocus(Window->TakeWidget());
    Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
    Mode.SetHideCursorDuringCapture(false);
    PC->SetInputMode(Mode);
    PC->SetShowMouseCursor(true);
    PC->bEnableClickEvents = true;
    PC->bEnableMouseOverEvents = true;
    PC->CurrentMouseCursor = EMouseCursor::Default;
    PC->SetIgnoreMoveInput(true);
    PC->SetIgnoreLookInput(true);

    StartLiveUpdates();
}

void UDynamicTimeTableRemoteCallObject::StartLiveUpdates()
{
    UWorld* World = GetWorld();
    if (!IsValid(World)) return;

    RequestSnapshot();
    World->GetTimerManager().ClearTimer(LiveUpdateTimerHandle);
    World->GetTimerManager().SetTimer(
        LiveUpdateTimerHandle,
        this,
        &UDynamicTimeTableRemoteCallObject::PollLiveSnapshot,
        1.5f,
        true);
}

void UDynamicTimeTableRemoteCallObject::PollLiveSnapshot()
{
    AFGPlayerController* PC = GetTypedOuter<AFGPlayerController>();
    UWorld* World = GetWorld();
    if (!IsValid(PC) || !IsValid(World)) return;

    TArray<UUserWidget*> Widgets;
    UWidgetBlueprintLibrary::GetAllWidgetsOfClass(
        PC, Widgets, UDynamicTimeTableWidget::StaticClass(), true);

    const bool bWindowOpen = Widgets.ContainsByPredicate(
        [](const UUserWidget* Widget)
        {
            return IsValid(Widget) && Widget->IsInViewport();
        });

    if (!bWindowOpen)
    {
        World->GetTimerManager().ClearTimer(LiveUpdateTimerHandle);
        return;
    }

    // Only what the open tab shows: the groups page needs the groups, the other two their rows.
    int32 Sections = EDTTSnapshotSection::Groups;
    for (UUserWidget* Widget : Widgets)
    {
        if (const UDynamicTimeTableWidget* Window = Cast<UDynamicTimeTableWidget>(Widget))
        {
            if (Window->IsInViewport()) { Sections = Window->GetRequestedSections(); break; }
        }
    }
    RequestSnapshot(Sections);
}
