#include "DynamicTimeTableWidget.h"

#include "DynamicTimeTableSubsystem.h"
#include "DynamicTimeTableRemoteCallObject.h"
#include "DTTItemNames.h"
#include "FGPlayerController.h"
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
    RefreshData();
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
    if (ActiveTab != 0) RefreshRuntimeDisplay();
}
void UDynamicTimeTableWidget::RefreshRuntimeDisplay()
{
    if (ActiveTab == 1) { RefreshDispatchDisplay(); return; }
    if (ActiveTab == 2) { RefreshTruckDisplay(); return; }
    if (ActiveTab == 3) return;
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
    case 1: return EDTTSnapshotSection::Dispatch;
    case 2: return EDTTSnapshotSection::Trucks;
    default: return EDTTSnapshotSection::Groups;
    }
}

void UDynamicTimeTableWidget::SetActiveTab(int32 Tab)
{
    ActiveTab = FMath::Clamp(Tab, 0, 3);
    if (Pages) Pages->SetActiveWidgetIndex(ActiveTab);
    const FLinearColor Active(0.2f, 0.8f, 1.0f, 1.0f);
    const FLinearColor Inactive(0.55f, 0.6f, 0.65f, 1.0f);
    if (GroupsTabButton) GroupsTabButton->SetBackgroundColor(ActiveTab == 0 ? Active : Inactive);
    if (DispatchTabButton) DispatchTabButton->SetBackgroundColor(ActiveTab == 1 ? Active : Inactive);
    if (TrucksTabButton) TrucksTabButton->SetBackgroundColor(ActiveTab == 2 ? Active : Inactive);
    if (SettingsTabButton) SettingsTabButton->SetBackgroundColor(ActiveTab == 3 ? Active : Inactive);
    // Recalculate rebuilds the group bookings of the balancer, which only the Groups tab shows.
    if (RecalculateButton) RecalculateButton->SetVisibility(ActiveTab == 0 ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
    // A remote client shows the last snapshot of this section until the next one arrives; ask now
    // rather than at the next 1.5 s tick.
    if (IsRemoteClient() && RemoteCallObject) RemoteCallObject->RequestSnapshot(GetRequestedSections());
    RefreshRuntimeDisplay();
}

void UDynamicTimeTableWidget::HandleGroupsTabClicked() { SetActiveTab(0); }
void UDynamicTimeTableWidget::HandleDispatchTabClicked() { SetActiveTab(1); }
void UDynamicTimeTableWidget::HandleTrucksTabClicked() { SetActiveTab(2); }

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
    for (int32 Index = 0; Index < Keys.Num(); ++Index)
    {
        if (Cards.IsValidIndex(Index) && Cards[Index].Root && Cards[Index].Key == Keys[Index])
        {
            FDTTCardSink Sink(WidgetTree, false, Cards[Index]);
            Fill(Index, Sink);
            // The shape changed without the key noticing: rebuild this card on the next refresh.
            if (Sink.Overran()) Cards[Index].Key.Reset();
            continue;
        }
        FDTTCardWidgets Fresh;
        Fresh.Key = Keys[Index];
        FDTTCardSink Sink(WidgetTree, true, Fresh);
        Fill(Index, Sink);
        UE_LOG(LogDynamicTimeTableWidget, Verbose, TEXT("CARD_BUILT Index=%d Key=%s"), Index, *Keys[Index]);
        if (Cards.IsValidIndex(Index))
        {
            if (Cards[Index].Root) Page->RemoveChild(Cards[Index].Root);
            Cards[Index] = MoveTemp(Fresh);
        }
        else
        {
            Cards.Add(MoveTemp(Fresh));
        }
        if (Cards[Index].Root)
        {
            Page->InsertChildAt(Index, Cards[Index].Root);
            FDTTCardSink::Pad<UScrollBoxSlot>(Cards[Index].Root, FMargin(4, 3));
        }
    }
    while (Cards.Num() > Keys.Num())
    {
        if (Cards.Last().Root) Page->RemoveChild(Cards.Last().Root);
        Cards.Pop();
    }
}
