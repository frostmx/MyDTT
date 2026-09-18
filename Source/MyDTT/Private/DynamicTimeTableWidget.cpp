#include "DynamicTimeTableWidget.h"

#include "DynamicTimeTableSubsystem.h"
#include "DynamicTimeTableRemoteCallObject.h"
#include "DTTItemNames.h"
#include "FGPlayerController.h"
#include "WheeledVehicles/FGWheeledVehicleIdentifier.h"
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
    if (ActiveTab == EDTTTab::Settings) return;
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
    default: return EDTTSnapshotSection::Groups;
    }
}

void UDynamicTimeTableWidget::HandleRowAction(const FDTTRowTarget& Target)
{
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

    // Everything below writes to the world. The handle is checked here rather than when the row was
    // drawn: the row is up to 1.5 s old, and a truck can be sold while it is on the screen.
    AFGWheeledVehicleIdentifier* Truck = Target.Truck.Get();
    if (!IsValid(Truck))
    {
        SetEditorStatus(TEXT("dTT: that truck is gone."), true);
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
    ActiveTab = (Tab >= EDTTTab::Groups && Tab < EDTTTab::Count) ? Tab : EDTTTab::Groups;
    if (Pages) Pages->SetActiveWidgetIndex(static_cast<int32>(ActiveTab));
    const FLinearColor Active(0.2f, 0.8f, 1.0f, 1.0f);
    const FLinearColor Inactive(0.55f, 0.6f, 0.65f, 1.0f);
    if (GroupsTabButton) GroupsTabButton->SetBackgroundColor(ActiveTab == EDTTTab::Groups ? Active : Inactive);
    if (DispatchTabButton) DispatchTabButton->SetBackgroundColor(ActiveTab == EDTTTab::Dispatch ? Active : Inactive);
    if (TrucksTabButton) TrucksTabButton->SetBackgroundColor(ActiveTab == EDTTTab::Trucks ? Active : Inactive);
    if (SettingsTabButton) SettingsTabButton->SetBackgroundColor(ActiveTab == EDTTTab::Settings ? Active : Inactive);
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

    // Left to right: everything before Index already sits where it belongs, so pulling a card out
    // of a later slot and inserting it here cannot disturb them. InsertChildAt makes a slot rather
    // than moving one, hence the removal first and the padding again after.
    for (int32 Index = 0; Index < Cards.Num(); ++Index)
    {
        UWidget* Root = Cards[Index].Root;
        if (!Root || Page->GetChildIndex(Root) == Index) continue;
        Page->RemoveChild(Root);
        Page->InsertChildAt(Index, Root);
        FDTTCardSink::Pad<UScrollBoxSlot>(Root, FMargin(4, 3));
    }
}
