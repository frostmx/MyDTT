#include "DynamicTimeTableWidget.h"

#include "DynamicTimeTableSubsystem.h"
#include "DynamicTimeTableRemoteCallObject.h"
#include "DTTItemNames.h"
#include "DTTCargoPaint.h"
#include "FGPlayerController.h"
#include "WheeledVehicles/FGWheeledVehicleIdentifier.h"
#include "WheeledVehicles/FGDockingStationIdentifier.h"
#include "Engine/Texture2D.h"
#include "Resources/FGItemDescriptor.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/CheckBox.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/ComboBoxString.h"
#include "Components/EditableTextBox.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/PanelWidget.h"
#include "Components/ProgressBar.h"
#include "Components/ScrollBox.h"
#include "Components/ScrollBoxSlot.h"
#include "Components/SizeBox.h"
#include "Components/Spacer.h"
#include "Components/SpinBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/WidgetSwitcher.h"
#include "Components/WidgetSwitcherSlot.h"
#include "Components/WrapBox.h"
#include "Components/WrapBoxSlot.h"

#include "DTTCardSink.h"

DEFINE_LOG_CATEGORY(LogDynamicTimeTableWidget);

using namespace DTTUI;

UDynamicTimeTableWidget::UDynamicTimeTableWidget(const FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer)
{
    mUseKeyboard = true;
    mUseMouse = true;
    mMouseLockMode = EMouseLockMode::LockOnCapture;
    mHideCursorDuringCapture = false;
    mDisablePlayerActions = true;
    mDisableBuildGunActions = true;
    mDisablePlayerEquipmentManagement = true;
    mDisableVisualizationModeActions = true;
    mFlushMouseKeysOnOpen = true;
    mSupportsStacking = false;
    mSupportsCaching = false;
    mRestoreFocusWhenLost = true;
}

void UDynamicTimeTableWidget::NativeOnInitialized()
{
    Super::NativeOnInitialized();
    // Before the tree: the controls built with a page - the Stations chips and sortable captions -
    // copy the delegate when they are bound, and a copy taken unbound stays unbound.
    if (!RowActionDelegate.IsBound()) RowActionDelegate.BindUObject(this, &UDynamicTimeTableWidget::HandleRowAction);
    if (WidgetTree && !WidgetTree->RootWidget) BuildWidgetTree();
}

void UDynamicTimeTableWidget::NativeConstruct()
{
    Super::NativeConstruct();
    bInitialFocusApplied = false;
    LocalLiveRefreshAccumulator = 0.0f;

    CloseButton->OnClicked.AddDynamic(this, &UDynamicTimeTableWidget::HandleCloseClicked);
    RecalculateButton->OnClicked.AddDynamic(this, &UDynamicTimeTableWidget::HandleRecalculateClicked);
    CreateGroupButton->OnClicked.AddDynamic(this, &UDynamicTimeTableWidget::HandleCreateGroupClicked);
    RenameGroupButton->OnClicked.AddDynamic(this, &UDynamicTimeTableWidget::HandleRenameGroupClicked);
    DeleteGroupButton->OnClicked.AddDynamic(this, &UDynamicTimeTableWidget::HandleDeleteGroupClicked);
    AddStationButton->OnClicked.AddDynamic(this, &UDynamicTimeTableWidget::HandleAddStationClicked);
    ApplySlotsButton->OnClicked.AddDynamic(this, &UDynamicTimeTableWidget::HandleApplySlotsClicked);
    RemoveStationButton->OnClicked.AddDynamic(this, &UDynamicTimeTableWidget::HandleRemoveStationClicked);
    GroupSelector->OnSelectionChanged.AddDynamic(this, &UDynamicTimeTableWidget::HandleGroupSelectionChanged);
    GroupStationSelector->OnSelectionChanged.AddDynamic(this, &UDynamicTimeTableWidget::HandleGroupStationSelectionChanged);
    GroupsTabButton->OnClicked.AddDynamic(this, &UDynamicTimeTableWidget::HandleGroupsTabClicked);
    DispatchTabButton->OnClicked.AddDynamic(this, &UDynamicTimeTableWidget::HandleDispatchTabClicked);
    TrucksTabButton->OnClicked.AddDynamic(this, &UDynamicTimeTableWidget::HandleTrucksTabClicked);
    StationsTabButton->OnClicked.AddDynamic(this, &UDynamicTimeTableWidget::HandleStationsTabClicked);
    TimetableTabButton->OnClicked.AddDynamic(this, &UDynamicTimeTableWidget::HandleTimetableTabClicked);
    SettingsTabButton->OnClicked.AddDynamic(this, &UDynamicTimeTableWidget::HandleSettingsTabClicked);
    SettingsSaveButton->OnClicked.AddDynamic(this, &UDynamicTimeTableWidget::HandleSettingsSaveClicked);
    SettingsDefaultsButton->OnClicked.AddDynamic(this, &UDynamicTimeTableWidget::HandleSettingsDefaultsClicked);
    for (USpinBox* Spin : { SettingSolidRequest.Get(), SettingFluidRequest.Get(), SettingRequestBuffer.Get(),
        SettingLeftoverHold.Get(), SettingTopUpInterval.Get(), SettingDispatchInterval.Get(), SettingMaxRequesters.Get(),
        SettingClaimTimeout.Get(), SettingOverdueMinutes.Get(), SettingIdleRecheck.Get(), SettingFuelCritical.Get(), SettingFuelSkip.Get(),
        SettingAutoNameDebounce.Get(), SettingAutoNameSweep.Get(), SettingAutoNameMaxNodes.Get(), SettingAutoNameMaxProducts.Get() })
    {
        if (Spin) Spin->OnValueChanged.AddDynamic(this, &UDynamicTimeTableWidget::HandleSettingSpinChanged);
    }
    for (UCheckBox* Check : { SettingExactQuantities.Get(), SettingFluidFullLoad.Get(), SettingDrainLeftovers.Get(), SettingUnloadAtDepot.Get(), SettingSkipProducer.Get(),
        SettingAutoName.Get() })
    {
        if (Check) Check->OnCheckStateChanged.AddDynamic(this, &UDynamicTimeTableWidget::HandleSettingCheckChanged);
    }
    if (SettingNamingLanguage) SettingNamingLanguage->OnSelectionChanged.AddDynamic(this, &UDynamicTimeTableWidget::HandleSettingComboChanged);
    SetDefaultFocusWidget(CloseButton);

    if (UDynamicTimeTableSubsystem* Subsystem = GetWorld()->GetSubsystem<UDynamicTimeTableSubsystem>())
        GroupsChangedHandle = Subsystem->OnGroupsChangedNative.AddUObject(this, &UDynamicTimeTableWidget::HandleLocalGroupsChanged);
    if (AFGPlayerController* PC = Cast<AFGPlayerController>(GetOwningPlayer()))
    {
        RemoteCallObject = Cast<UDynamicTimeTableRemoteCallObject>(PC->GetRemoteCallObjectOfClass(UDynamicTimeTableRemoteCallObject::StaticClass()));
        if (!RemoteCallObject) RemoteCallObject = Cast<UDynamicTimeTableRemoteCallObject>(PC->RegisterRemoteCallObjectClass(UDynamicTimeTableRemoteCallObject::StaticClass()));
        if (RemoteCallObject)
        {
            SnapshotChangedHandle = RemoteCallObject->OnSnapshotChanged.AddUObject(this, &UDynamicTimeTableWidget::HandleRemoteSnapshot);
            OperationResultHandle = RemoteCallObject->OnOperationResult.AddUObject(this, &UDynamicTimeTableWidget::HandleRemoteResult);
            if (IsRemoteClient()) RemoteCallObject->RequestSnapshot();
        }
    }
    if (!RowActionDelegate.IsBound()) RowActionDelegate.BindUObject(this, &UDynamicTimeTableWidget::HandleRowAction);
    RefreshData();
    ApplyWindowSize();
    SetActiveTab(ActiveTab);
    if (!IsRemoteClient())
    {
        if (UDynamicTimeTableSubsystem* Subsystem = GetWorld()->GetSubsystem<UDynamicTimeTableSubsystem>())
            LastAppliedConfigurationRevision = Subsystem->GetConfigurationRevision();
    }
    UE_LOG(LogDynamicTimeTableWidget, Verbose, TEXT("Virtual /dtt window opened with group editor."));
}

void UDynamicTimeTableWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
    Super::NativeTick(MyGeometry, InDeltaTime);

    // Resolution and DPI can change under an open window; ApplyWindowSize returns at once
    // unless the answer differs, so this is not a relayout every frame.
    ApplyWindowSize();

    // Remote clients are refreshed by the RCO snapshot timer. Standalone and
    // listen-host widgets poll only their existing runtime text/color widgets.
    // This keeps live values current without rebuilding the widget tree or
    // closing combo-box popups.
    if (!IsRemoteClient())
    {
        LocalLiveRefreshAccumulator += InDeltaTime;
        if (LocalLiveRefreshAccumulator >= 1.5f)
        {
            LocalLiveRefreshAccumulator = 0.0f;
            RefreshRuntimeDisplay();
        }
    }

    if (AFGPlayerController* Player = Cast<AFGPlayerController>(GetOwningPlayer()))
    {
        Player->SetShowMouseCursor(true);
        Player->bEnableClickEvents = true;
        Player->bEnableMouseOverEvents = true;
        Player->CurrentMouseCursor = EMouseCursor::Default;
        if (!bInitialFocusApplied)
        {
            FInputModeUIOnly InputMode;
            InputMode.SetWidgetToFocus(TakeWidget());
            InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
            Player->SetInputMode(InputMode);
            SetUserFocus(Player);
            SetKeyboardFocus();
            Player->SetIgnoreMoveInput(true);
            Player->SetIgnoreLookInput(true);
            bInitialFocusApplied = true;
            UE_LOG(LogDynamicTimeTableWidget, Verbose, TEXT("Virtual /dtt window received initial UI focus."));
        }
    }
}

void UDynamicTimeTableWidget::NativeDestruct()
{
    if (UDynamicTimeTableSubsystem* Subsystem = GetWorld()->GetSubsystem<UDynamicTimeTableSubsystem>())
        if (GroupsChangedHandle.IsValid()) Subsystem->OnGroupsChangedNative.Remove(GroupsChangedHandle);
    GroupsChangedHandle.Reset();
    if (RemoteCallObject)
    {
        if (SnapshotChangedHandle.IsValid()) RemoteCallObject->OnSnapshotChanged.Remove(SnapshotChangedHandle);
        if (OperationResultHandle.IsValid()) RemoteCallObject->OnOperationResult.Remove(OperationResultHandle);
    }
    SnapshotChangedHandle.Reset(); OperationResultHandle.Reset(); RemoteCallObject = nullptr;

    if (AFGPlayerController* Player = Cast<AFGPlayerController>(GetOwningPlayer()))
    {
        FInputModeGameOnly InputMode;
        Player->SetInputMode(InputMode);
        Player->SetShowMouseCursor(false);
        Player->SetIgnoreMoveInput(false);
        Player->SetIgnoreLookInput(false);
    }
    UE_LOG(LogDynamicTimeTableWidget, Verbose, TEXT("Virtual /dtt window closed."));
    Super::NativeDestruct();
}

void UDynamicTimeTableWidget::OnEscapePressed_Implementation() { CloseWindow(); }

bool UDynamicTimeTableWidget::IsRemoteClient() const{return GetWorld()&&GetWorld()->GetNetMode()==NM_Client;}
const TArray<FDTTStationGroup>& UDynamicTimeTableWidget::ViewGroups()const{if(IsRemoteClient()&&RemoteCallObject)return RemoteCallObject->GetSnapshot().Groups;static TArray<FDTTStationGroup> Empty;if(auto*S=GetWorld()->GetSubsystem<UDynamicTimeTableSubsystem>())return S->GetStationGroups();return Empty;}
void UDynamicTimeTableWidget::HandleLocalGroupsChanged()
{
    UDynamicTimeTableSubsystem* Subsystem = GetWorld()
        ? GetWorld()->GetSubsystem<UDynamicTimeTableSubsystem>()
        : nullptr;
    if (!Subsystem) return;

    const int32 Revision = Subsystem->GetConfigurationRevision();
    if (Revision != LastAppliedConfigurationRevision)
    {
        RefreshData();
        LastAppliedConfigurationRevision = Revision;
        return;
    }

    RefreshRuntimeDisplay();
}
void UDynamicTimeTableWidget::HandleRemoteSnapshot()
{
    if (!RemoteCallObject) return;

    const int32 SnapshotRevision = RemoteCallObject->GetSnapshot().Revision;
    const bool bConfigurationChanged =
        !bHasAppliedRemoteConfiguration ||
        bForceFullRefreshOnNextSnapshot ||
        SnapshotRevision != LastAppliedConfigurationRevision;

    if (bConfigurationChanged)
    {
        RefreshData();
        LastAppliedConfigurationRevision = SnapshotRevision;
        bHasAppliedRemoteConfiguration = true;
        bForceFullRefreshOnNextSnapshot = false;
        return;
    }

    RefreshRuntimeDisplay();
}
void UDynamicTimeTableWidget::HandleRemoteResult(bool O,const FString&M)
{
    if (O) bForceFullRefreshOnNextSnapshot = true;
    SetEditorStatus(M,!O);
    // The server's answer to a settings request is the snapshot that follows; a refused one
    // (unchanged) leaves the controls as they are, an applied one refills them.
    if (O) bSettingsEdited = false;
}
void UDynamicTimeTableWidget::RefreshData()
{
    RebuildRuntimeDisplay();
    RefreshEditorControls();
    RefreshSettingsControls(false);
    if (ActiveTab != EDTTTab::Groups) RefreshRuntimeDisplay();
}
void UDynamicTimeTableWidget::RefreshRuntimeDisplay()
{
    if (ActiveTab == EDTTTab::Dispatch) { RefreshDispatchDisplay(); return; }
    if (ActiveTab == EDTTTab::Trucks) { RefreshTruckDisplay(); return; }
    if (ActiveTab == EDTTTab::Stations) { RefreshStationDisplay(); return; }
    if (ActiveTab == EDTTTab::Timetable) { RefreshTimetableDisplay(); return; }
    if (ActiveTab == EDTTTab::Settings)
    {
        SetStatusLine(DTTUI::FDTTStatusLine().Words(TEXT("Settings are the server's: an edit here is a proposal, and the answer comes back in the line at the bottom.")), FString());
        return;
    }
    if (!GroupScrollBox) return;

    if (!RuntimeLayoutMatches())
    {
        RebuildRuntimeDisplay();
        return;
    }

    UpdateRuntimeDisplayInPlace();
}
void UDynamicTimeTableWidget::HandleCloseClicked(){CloseWindow();}void UDynamicTimeTableWidget::CloseWindow(){AFGPlayerController*P=Cast<AFGPlayerController>(GetOwningPlayer());RemoveFromParent();if(IsValid(P)){FInputModeGameOnly I;P->SetInputMode(I);P->SetShowMouseCursor(false);P->SetIgnoreMoveInput(false);P->SetIgnoreLookInput(false);}}

// ---------------------------------------------------------------------------
// Dispatch and Trucks tabs: one text row per dispatch station / truck, rebuilt only when the
// set of rows changes and updated in place otherwise, like the groups page.
// ---------------------------------------------------------------------------

int32 UDynamicTimeTableWidget::GetRequestedSections() const
{
    switch (ActiveTab)
    {
    case EDTTTab::Dispatch: return EDTTSnapshotSection::Dispatch;
    case EDTTTab::Trucks: return EDTTSnapshotSection::Trucks;
    case EDTTTab::Stations: return EDTTSnapshotSection::Stations;
    case EDTTTab::Timetable: return EDTTSnapshotSection::Timetable;
    default: return EDTTSnapshotSection::Groups;
    }
}

void UDynamicTimeTableWidget::HandleRowAction(const FDTTRowTarget& Target)
{
    const bool bWasArmed = IsArmed(Target.Action, Target.CardKey);
    const EDTTRowAction PreviousArmed = ArmedAction;
    ArmedAction = EDTTRowAction::None;
    ArmedKey.Reset();

    // Opening a card is the window's own business: it goes into the card's key and the page
    // redraws that one row.
    if (Target.Action == EDTTRowAction::ToggleExpandDispatch)
    {
        ExpandedDispatchKey = ExpandedDispatchKey == Target.CardKey ? FString() : Target.CardKey;
        RefreshDispatchDisplay();
        return;
    }
    if (Target.Action == EDTTRowAction::ToggleExpandTruck)
    {
        ExpandedTruckKey = ExpandedTruckKey == Target.CardKey ? FString() : Target.CardKey;
        RefreshTruckDisplay();
        return;
    }
    if (Target.Action == EDTTRowAction::ToggleExpandStation)
    {
        ExpandedStationKey = ExpandedStationKey == Target.CardKey ? FString() : Target.CardKey;
        RefreshStationDisplay();
        return;
    }
    // The chips and captions of the Stations table: what it shows, and in what order.
    if (Target.Action == EDTTRowAction::StationFilter)
    {
        StationRoleFilter = EDTTStationRoleFilter(FMath::Clamp(Target.Value, 0, int32(EDTTStationRoleFilter::Count) - 1));
        RefreshStationDisplay();
        return;
    }
    if (Target.Action == EDTTRowAction::StationSort)
    {
        const EDTTStationSort Column = EDTTStationSort(FMath::Clamp(Target.Value, 0, int32(EDTTStationSort::Count) - 1));
        if (StationSort == Column) bStationSortDescending = !bStationSortDescending;
        else { StationSort = Column; bStationSortDescending = false; }
        RefreshStationDisplay();
        return;
    }
    if (Target.Action == EDTTRowAction::StationSelectAll)
    {
        bool bAll = ShownStationRequesters.Num() > 0;
        for (const FGuid& Guid : ShownStationRequesters) if (!SelectedStations.Contains(Guid)) { bAll = false; break; }
        for (const FGuid& Guid : ShownStationRequesters)
        {
            if (bAll) SelectedStations.Remove(Guid);
            else SelectedStations.Add(Guid);
        }
        RefreshStationDisplay();
        return;
    }
    // A tick is the window's own business as well: the selection lives here, and the box is only
    // ever told what it is.
    if (Target.Action == EDTTRowAction::StationSelect)
    {
        if (SelectedStations.Contains(Target.Guid)) SelectedStations.Remove(Target.Guid);
        else SelectedStations.Add(Target.Guid);
        RefreshStationDisplay();
        return;
    }

    // The station operations: the same shape as the truck ones below, against the dock handle.
    if (Target.Action == EDTTRowAction::StationStayOn || Target.Action == EDTTRowAction::StationStayOff
        || Target.Action == EDTTRowAction::StationClearClaim || Target.Action == EDTTRowAction::StationSendNow)
    {
        AFGDockingStationIdentifier* Dock = Target.Dock.Get();
        if (!IsValid(Dock))
        {
            SetEditorStatus(TEXT("dTT: that station is gone."), true);
            if (PreviousArmed != EDTTRowAction::None) RefreshRuntimeDisplay();
            return;
        }
        if (IsRemoteClient())
        {
            if (!RemoteCallObject) { SetEditorStatus(TEXT("dTT: no connection to the server."), true); return; }
            switch (Target.Action)
            {
            case EDTTRowAction::StationStayOn: RemoteCallObject->RequestSetDockStay(Dock, true); break;
            case EDTTRowAction::StationStayOff: RemoteCallObject->RequestSetDockStay(Dock, false); break;
            case EDTTRowAction::StationClearClaim: RemoteCallObject->RequestClearDockClaim(Target.Guid); break;
            case EDTTRowAction::StationSendNow: RemoteCallObject->RequestSendTruckNow(Dock); break;
            default: return;
            }
            SetEditorStatus(TEXT("dTT: asked the server..."));
            return;
        }
        UDynamicTimeTableSubsystem* StationSubsystem = GetWorld() ? GetWorld()->GetSubsystem<UDynamicTimeTableSubsystem>() : nullptr;
        if (!StationSubsystem) { SetEditorStatus(TEXT("dTT: subsystem unavailable."), true); return; }
        FString StationMessage(TEXT("dTT: nothing to do."));
        bool bStationOk = false;
        switch (Target.Action)
        {
        case EDTTRowAction::StationStayOn: bStationOk = StationSubsystem->SetDockStay(Dock, true, StationMessage); break;
        case EDTTRowAction::StationStayOff: bStationOk = StationSubsystem->SetDockStay(Dock, false, StationMessage); break;
        case EDTTRowAction::StationClearClaim: bStationOk = StationSubsystem->ClearRequesterClaim(Target.Guid, StationMessage); break;
        case EDTTRowAction::StationSendNow: bStationOk = StationSubsystem->SendTruckToRequester(Dock, StationMessage); break;
        default: return;
        }
        SetEditorStatus(StationMessage, !bStationOk);
        RefreshRuntimeDisplay();
        return;
    }

    // Switching an autopilot off hands the truck back to the player and the mod stops planning
    // for it, so that one asks twice. Everything else a row offers is undone by clicking the
    // opposite, and a confirmation on all of them would only teach the player to click through.
    if (Target.Action == EDTTRowAction::TruckAutopilotOff && !bWasArmed)
    {
        ArmedAction = Target.Action;
        ArmedKey = Target.CardKey;
        SetEditorStatus(TEXT("dTT: click again to switch the autopilot off - the mod stops planning for that truck."));
        RefreshRuntimeDisplay();
        return;
    }

    // Everything below writes to the world. The handle is checked here rather than when the row was
    // drawn: the row is up to 1.5 s old, and a truck can be sold while it is on the screen.
    AFGWheeledVehicleIdentifier* Truck = Target.Truck.Get();
    if (!IsValid(Truck))
    {
        SetEditorStatus(TEXT("dTT: that truck is gone."), true);
        if (PreviousArmed != EDTTRowAction::None) RefreshRuntimeDisplay();
        return;
    }

    if (IsRemoteClient())
    {
        if (!RemoteCallObject)
        {
            SetEditorStatus(TEXT("dTT: no connection to the server."), true);
            return;
        }
        switch (Target.Action)
        {
        case EDTTRowAction::TruckPark: RemoteCallObject->RequestParkTruck(Truck); break;
        case EDTTRowAction::TruckRelease: RemoteCallObject->RequestReleaseTruck(Truck); break;
        case EDTTRowAction::TruckAutopilotOn: RemoteCallObject->RequestSetAutopilot(Truck, true); break;
        case EDTTRowAction::TruckAutopilotOff: RemoteCallObject->RequestSetAutopilot(Truck, false); break;
        case EDTTRowAction::TruckRebuildLap: RemoteCallObject->RequestRebuildLap(Truck); break;
        default: return;
        }
        // The answer and the fresh rows both come back from the server; nothing to say yet.
        SetEditorStatus(TEXT("dTT: asked the server..."));
        return;
    }

    UDynamicTimeTableSubsystem* Subsystem = GetWorld() ? GetWorld()->GetSubsystem<UDynamicTimeTableSubsystem>() : nullptr;
    if (!Subsystem)
    {
        SetEditorStatus(TEXT("dTT: subsystem unavailable."), true);
        return;
    }
    FString Message(TEXT("dTT: nothing to do."));
    bool bOk = false;
    switch (Target.Action)
    {
    case EDTTRowAction::TruckPark: bOk = Subsystem->ParkTruck(Truck, Message); break;
    case EDTTRowAction::TruckRelease: bOk = Subsystem->ReleaseTruck(Truck, Message); break;
    case EDTTRowAction::TruckAutopilotOn: bOk = Subsystem->SetTruckAutopilot(Truck, true, Message); break;
    case EDTTRowAction::TruckAutopilotOff: bOk = Subsystem->SetTruckAutopilot(Truck, false, Message); break;
    case EDTTRowAction::TruckRebuildLap: bOk = Subsystem->RebuildTruckLap(Truck, Message); break;
    default: return;
    }
    SetEditorStatus(Message, !bOk);
    RefreshRuntimeDisplay();
}

void UDynamicTimeTableWidget::SetActiveTab(EDTTTab Tab)
{
    ActiveTab = (Tab >= EDTTTab::Dispatch && Tab < EDTTTab::Count) ? Tab : EDTTTab::Dispatch;
    if (Pages) Pages->SetActiveWidgetIndex(static_cast<int32>(ActiveTab));
    auto Style = [this](UButton* Button, EDTTTab Which)
    {
        DTTUI::StyleButton(Button, ActiveTab == Which ? DTTUI::EDTTButton::TabOn : DTTUI::EDTTButton::Tab);
    };
    Style(DispatchTabButton, EDTTTab::Dispatch);
    Style(TrucksTabButton, EDTTTab::Trucks);
    Style(StationsTabButton, EDTTTab::Stations);
    Style(TimetableTabButton, EDTTTab::Timetable);
    Style(GroupsTabButton, EDTTTab::Groups);
    Style(SettingsTabButton, EDTTTab::Settings);
    // Recalculate rebuilds the group bookings of the balancer, which only the Groups tab shows.
    if (RecalculateButton) RecalculateButton->SetVisibility(ActiveTab == EDTTTab::Groups ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
    // A remote client shows the last snapshot of this section until the next one arrives; ask now
    // rather than at the next 1.5 s tick.
    if (IsRemoteClient() && RemoteCallObject) RemoteCallObject->RequestSnapshot(GetRequestedSections());
    RefreshRuntimeDisplay();
}

void UDynamicTimeTableWidget::HandleGroupsTabClicked() { SetActiveTab(EDTTTab::Groups); }
void UDynamicTimeTableWidget::HandleDispatchTabClicked() { SetActiveTab(EDTTTab::Dispatch); }
void UDynamicTimeTableWidget::HandleTrucksTabClicked() { SetActiveTab(EDTTTab::Trucks); }
void UDynamicTimeTableWidget::HandleStationsTabClicked() { SetActiveTab(EDTTTab::Stations); }
void UDynamicTimeTableWidget::HandleTimetableTabClicked() { SetActiveTab(EDTTTab::Timetable); }

void UDynamicTimeTableWidget::SetStatusLine(const DTTUI::FDTTStatusLine& Line, const FString& Source)
{
    for (int32 Index = 0; Index < StatusParts.Num(); ++Index)
    {
        UTextBlock* Part = StatusParts[Index].Get();
        if (!Part) continue;
        if (Line.Parts.IsValidIndex(Index))
        {
            Part->SetText(FText::FromString(Line.Parts[Index].Key));
            Part->SetColorAndOpacity(FSlateColor(Line.Parts[Index].Value));
            Part->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
        }
        else
        {
            Part->SetVisibility(ESlateVisibility::Collapsed);
        }
    }
    if (SummaryText)
    {
        SummaryText->SetText(FText::FromString(Source.IsEmpty()
            ? (IsRemoteClient() ? FString::Printf(TEXT("server snapshot  %s  protocol %d"), *FString::Chr(0x00B7), UDynamicTimeTableRemoteCallObject::ProtocolVersion) : FString(TEXT("live")))
            : Source));
    }
}

FLinearColor UDynamicTimeTableWidget::TileColorFor(const TSoftClassPtr<UFGItemDescriptor>& ItemClass)
{
    if (ItemClass.IsNull()) return DTTUI::DTTTrack;
    const FSoftObjectPath Path = ItemClass.ToSoftObjectPath();
    if (const FLinearColor* Cached = TileColorCache.Find(Path)) return *Cached;
    // The same table the trucks are painted from, so a row's tile and the truck on the road match.
    UClass* Loaded = ItemClass.LoadSynchronous();
    const FLinearColor Colour = Loaded ? FDTTCargoPaint::ColorsFor(Loaded).Primary : DTTUI::DTTTrack;
    TileColorCache.Add(Path, Colour);
    return Colour;
}

UTexture2D* UDynamicTimeTableWidget::IconFor(const TSoftClassPtr<UFGItemDescriptor>& ItemClass)
{
    if (ItemClass.IsNull()) return nullptr;
    const FSoftObjectPath Path = ItemClass.ToSoftObjectPath();
    if (const TObjectPtr<UTexture2D>* Cached = IconCache.Find(Path)) return Cached->Get();
    // Descriptors are small and the game keeps them around; a synchronous load is a one-off per item.
    UClass* Loaded = ItemClass.LoadSynchronous();
    UTexture2D* Texture = Loaded ? UFGItemDescriptor::GetSmallIcon(Loaded) : nullptr;
    IconCache.Add(Path, Texture);
    return Texture;
}

void UDynamicTimeTableWidget::SyncCards(UScrollBox* Page, TArray<FDTTCardWidgets>& Cards, const TArray<FString>& Keys,
    TFunctionRef<void(int32, FDTTCardSink&)> Fill)
{
    if (!Page) return;

    // Cards are claimed by key, not by position. Matching by position meant one dock appearing in
    // the middle of the page put every later key against the wrong card and rebuilt the whole tail;
    // the keys carry a guid, so a map of them is enough to move the survivors into place instead.
    TMap<FString, int32> Surviving;
    Surviving.Reserve(Cards.Num());
    for (int32 Index = 0; Index < Cards.Num(); ++Index)
    {
        if (Cards[Index].Root && !Cards[Index].Key.IsEmpty()) Surviving.Add(Cards[Index].Key, Index);
    }

    TArray<FDTTCardWidgets> Next;
    Next.SetNum(Keys.Num());
    TBitArray<> Claimed(false, Cards.Num());

    for (int32 Index = 0; Index < Keys.Num(); ++Index)
    {
        if (const int32* Found = Surviving.Find(Keys[Index]))
        {
            Claimed[*Found] = true;
            Next[Index] = MoveTemp(Cards[*Found]);
            FDTTCardSink Sink(WidgetTree, false, Next[Index]);
            Sink.RowAction = RowActionDelegate;
            Fill(Index, Sink);
            // The shape changed without the key noticing: rebuild this card on the next refresh.
            // Say so, because such a card then rebuilds every 1.5 s for as long as the window is
            // open, which looks from the outside like the page flickering and left no trace at all.
            if (Sink.Overran())
            {
                UE_LOG(LogDynamicTimeTableWidget, Warning,
                    TEXT("CARD_OVERRAN Index=%d Key=%s Texts=%d/%d Borders=%d/%d Bars=%d/%d Images=%d/%d"),
                    Index, *Keys[Index],
                    Sink.TextIndex, Next[Index].Texts.Num(), Sink.BorderIndex, Next[Index].Borders.Num(),
                    Sink.BarIndex, Next[Index].Bars.Num(), Sink.ImageIndex, Next[Index].Images.Num());
                Next[Index].Key.Reset();
            }
            continue;
        }
        Next[Index].Key = Keys[Index];
        FDTTCardSink Sink(WidgetTree, true, Next[Index]);
        Sink.RowAction = RowActionDelegate;
        Fill(Index, Sink);
        UE_LOG(LogDynamicTimeTableWidget, Verbose, TEXT("CARD_BUILT Index=%d Key=%s"), Index, *Keys[Index]);
    }

    for (int32 Index = 0; Index < Cards.Num(); ++Index)
    {
        if (!Claimed[Index] && Cards[Index].Root) Page->RemoveChild(Cards[Index].Root);
    }
    Cards = MoveTemp(Next);

    // A UMG panel does not reorder what it draws. InsertChildAt is AddChild plus ShiftChild, and
    // ShiftChild moves the slot in the panel's own array while the live Slate box keeps the order
    // things were added in - so a rebuilt card went to the bottom of the page whatever index it
    // was given, and the row a player clicked flew down the list. Only removing and adding again
    // reaches Slate. So: find the first card that is not where it is drawn, take the tail off the
    // page and put it back in order. A page whose cards all kept their places does nothing at all,
    // which is every refresh that rebuilt no card.
    int32 First = 0;
    while (First < Cards.Num() && Cards[First].Root && Page->GetChildAt(First) == Cards[First].Root) ++First;
    for (int32 Index = First; Index < Cards.Num(); ++Index)
    {
        if (Cards[Index].Root) Page->RemoveChild(Cards[Index].Root);
    }
    for (int32 Index = First; Index < Cards.Num(); ++Index)
    {
        UWidget* Root = Cards[Index].Root;
        if (!Root) continue;
        Page->AddChild(Root);
        FDTTCardSink::Pad<UScrollBoxSlot>(Root, FMargin(2, 1));
    }
}
