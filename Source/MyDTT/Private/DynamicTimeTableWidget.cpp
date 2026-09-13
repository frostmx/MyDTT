#include "DynamicTimeTableWidget.h"

#include "DynamicTimeTableSubsystem.h"
#include "DynamicTimeTableRemoteCallObject.h"
#include "FGPlayerController.h"
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
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/Spacer.h"
#include "Components/SpinBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/WidgetSwitcher.h"
#include "Components/WidgetSwitcherSlot.h"

DEFINE_LOG_CATEGORY_STATIC(LogDynamicTimeTableWidget, Log, All);

namespace
{
    UTextBlock* MakeText(UWidgetTree* Tree, const FString& Value, int32 Size, const FLinearColor& Color)
    {
        UTextBlock* Text = Tree->ConstructWidget<UTextBlock>();
        Text->SetText(FText::FromString(Value));
        Text->SetColorAndOpacity(FSlateColor(Color));
        FSlateFontInfo Font = Text->GetFont();
        Font.Size = Size;
        Text->SetFont(Font);
        return Text;
    }

    UButton* MakeButton(UWidgetTree* Tree, const FString& Label)
    {
        UButton* Button = Tree->ConstructWidget<UButton>();
        Button->SetContent(MakeText(Tree, Label, 15, FLinearColor::White));
        return Button;
    }

    UHorizontalBoxSlot* AddRowItem(UHorizontalBox* Row, UWidget* Widget, float Padding = 5.0f, bool bFill = false)
    {
        UHorizontalBoxSlot* Slot = Row->AddChildToHorizontalBox(Widget);
        Slot->SetPadding(FMargin(Padding));
        Slot->SetVerticalAlignment(VAlign_Center);
        if (bFill) Slot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
        return Slot;
    }
}

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
    for (USpinBox* Spin : { SettingLeftoverHold.Get(), SettingTopUpInterval.Get(), SettingDispatchInterval.Get(), SettingMaxRequesters.Get(),
        SettingClaimTimeout.Get(), SettingIdleRecheck.Get(), SettingFuelCritical.Get(), SettingFuelSkip.Get() })
    {
        if (Spin) Spin->OnValueChanged.AddDynamic(this, &UDynamicTimeTableWidget::HandleSettingSpinChanged);
    }
    for (UCheckBox* Check : { SettingUnloadAtDepot.Get(), SettingSkipProducer.Get() })
    {
        if (Check) Check->OnCheckStateChanged.AddDynamic(this, &UDynamicTimeTableWidget::HandleSettingCheckChanged);
    }
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

void UDynamicTimeTableWidget::BuildWidgetTree()
{
    UCanvasPanel* Root = WidgetTree->ConstructWidget<UCanvasPanel>();
    WidgetTree->RootWidget = Root;

    UBorder* Dimmer = WidgetTree->ConstructWidget<UBorder>();
    Dimmer->SetBrushColor(FLinearColor(0,0,0,0.72f));
    UCanvasPanelSlot* DimmerSlot = Root->AddChildToCanvas(Dimmer);
    DimmerSlot->SetAnchors(FAnchors(0,0,1,1)); DimmerSlot->SetOffsets(FMargin(0));

    USizeBox* WindowSize = WidgetTree->ConstructWidget<USizeBox>();
    WindowSize->SetWidthOverride(1120); WindowSize->SetHeightOverride(820);
    UCanvasPanelSlot* WindowSlot = Root->AddChildToCanvas(WindowSize);
    WindowSlot->SetAnchors(FAnchors(0.5f)); WindowSlot->SetAlignment(FVector2D(0.5f));
    WindowSlot->SetSize(FVector2D(1120,820));

    UBorder* WindowBorder = WidgetTree->ConstructWidget<UBorder>();
    WindowBorder->SetBrushColor(FLinearColor(0.035f,0.055f,0.07f,0.98f));
    WindowBorder->SetPadding(FMargin(22)); WindowSize->SetContent(WindowBorder);
    UVerticalBox* Main = WidgetTree->ConstructWidget<UVerticalBox>(); WindowBorder->SetContent(Main);

    UHorizontalBox* Header = WidgetTree->ConstructWidget<UHorizontalBox>();
    Main->AddChildToVerticalBox(Header)->SetPadding(FMargin(0,0,0,8));
    AddRowItem(Header, MakeText(WidgetTree,TEXT("DynamicTimeTable"),30,FLinearColor(0.2f,0.8f,1)),5,true);
    CloseButton = MakeButton(WidgetTree,TEXT("Close")); AddRowItem(Header,CloseButton);

    SummaryText = MakeText(WidgetTree,TEXT("Loading..."),16,FLinearColor(0.78f,0.85f,0.9f));
    Main->AddChildToVerticalBox(SummaryText)->SetPadding(FMargin(5));

    // Four pages in one window; the tab row switches them, the active one is tinted.
    UHorizontalBox* Tabs = WidgetTree->ConstructWidget<UHorizontalBox>();
    Main->AddChildToVerticalBox(Tabs)->SetPadding(FMargin(5,0,5,4));
    GroupsTabButton = MakeButton(WidgetTree,TEXT("Groups")); AddRowItem(Tabs,GroupsTabButton,3);
    DispatchTabButton = MakeButton(WidgetTree,TEXT("Dispatch")); AddRowItem(Tabs,DispatchTabButton,3);
    TrucksTabButton = MakeButton(WidgetTree,TEXT("Trucks")); AddRowItem(Tabs,TrucksTabButton,3);
    SettingsTabButton = MakeButton(WidgetTree,TEXT("Settings")); AddRowItem(Tabs,SettingsTabButton,3);

    Pages = WidgetTree->ConstructWidget<UWidgetSwitcher>();
    UVerticalBoxSlot* PagesSlot = Main->AddChildToVerticalBox(Pages);
    PagesSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));

    UVerticalBox* GroupsPage = WidgetTree->ConstructWidget<UVerticalBox>();
    Pages->AddChild(GroupsPage);
    BuildEditorPanel(GroupsPage);
    GroupScrollBox = WidgetTree->ConstructWidget<UScrollBox>();
    UVerticalBoxSlot* ScrollSlot = GroupsPage->AddChildToVerticalBox(GroupScrollBox);
    ScrollSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill)); ScrollSlot->SetPadding(FMargin(5,8));

    DispatchScrollBox = WidgetTree->ConstructWidget<UScrollBox>();
    Pages->AddChild(DispatchScrollBox);
    TruckScrollBox = WidgetTree->ConstructWidget<UScrollBox>();
    Pages->AddChild(TruckScrollBox);
    UVerticalBox* SettingsPage = WidgetTree->ConstructWidget<UVerticalBox>();
    Pages->AddChild(SettingsPage);
    BuildSettingsPanel(SettingsPage);
    Pages->SetActiveWidgetIndex(0);

    UHorizontalBox* Footer = WidgetTree->ConstructWidget<UHorizontalBox>();
    Main->AddChildToVerticalBox(Footer)->SetHorizontalAlignment(HAlign_Right);
    RecalculateButton = MakeButton(WidgetTree,TEXT("Recalculate bookings")); AddRowItem(Footer,RecalculateButton);
    UButton* FooterClose = MakeButton(WidgetTree,TEXT("Close"));
    FooterClose->OnClicked.AddDynamic(this,&UDynamicTimeTableWidget::HandleCloseClicked); AddRowItem(Footer,FooterClose);
}

void UDynamicTimeTableWidget::BuildEditorPanel(UVerticalBox* Main)
{
    UBorder* EditorBorder = WidgetTree->ConstructWidget<UBorder>();
    EditorBorder->SetBrushColor(FLinearColor(0.08f,0.12f,0.15f,1)); EditorBorder->SetPadding(FMargin(10));
    Main->AddChildToVerticalBox(EditorBorder)->SetPadding(FMargin(5,5,5,8));
    UVerticalBox* Editor = WidgetTree->ConstructWidget<UVerticalBox>(); EditorBorder->SetContent(Editor);
    Editor->AddChildToVerticalBox(MakeText(WidgetTree,TEXT("Group configuration"),19,FLinearColor(0.2f,0.8f,1)));

    UHorizontalBox* CreateRow = WidgetTree->ConstructWidget<UHorizontalBox>(); Editor->AddChildToVerticalBox(CreateRow);
    NewGroupNameInput = WidgetTree->ConstructWidget<UEditableTextBox>(); NewGroupNameInput->SetHintText(FText::FromString(TEXT("New group name")));
    AddRowItem(CreateRow,NewGroupNameInput,5,true);
    NewGroupKindSelector=WidgetTree->ConstructWidget<UComboBoxString>();
    NewGroupKindSelector->AddOption(TEXT("Trains")); NewGroupKindSelector->AddOption(TEXT("Trucks"));
    NewGroupKindSelector->SetSelectedIndex(0); AddRowItem(CreateRow,NewGroupKindSelector);
    CreateGroupButton=MakeButton(WidgetTree,TEXT("Create group")); AddRowItem(CreateRow,CreateGroupButton);

    UHorizontalBox* GroupRow = WidgetTree->ConstructWidget<UHorizontalBox>(); Editor->AddChildToVerticalBox(GroupRow);
    GroupSelector=WidgetTree->ConstructWidget<UComboBoxString>(); AddRowItem(GroupRow,GroupSelector,5,true);
    GroupNameInput=WidgetTree->ConstructWidget<UEditableTextBox>(); GroupNameInput->SetHintText(FText::FromString(TEXT("Group name")));
    AddRowItem(GroupRow,GroupNameInput,5,true); RenameGroupButton=MakeButton(WidgetTree,TEXT("Rename")); AddRowItem(GroupRow,RenameGroupButton);
    DeleteGroupButton=MakeButton(WidgetTree,TEXT("Delete")); AddRowItem(GroupRow,DeleteGroupButton);

    UHorizontalBox* AddStationRow=WidgetTree->ConstructWidget<UHorizontalBox>(); Editor->AddChildToVerticalBox(AddStationRow);
    AvailableStationSelector=WidgetTree->ConstructWidget<UComboBoxString>(); AddRowItem(AddStationRow,AvailableStationSelector,5,true);
    AddStationButton=MakeButton(WidgetTree,TEXT("Add station")); AddRowItem(AddStationRow,AddStationButton);

    UHorizontalBox* StationRow=WidgetTree->ConstructWidget<UHorizontalBox>(); Editor->AddChildToVerticalBox(StationRow);
    GroupStationSelector=WidgetTree->ConstructWidget<UComboBoxString>(); AddRowItem(StationRow,GroupStationSelector,5,true);
    TargetSlotsInput=WidgetTree->ConstructWidget<USpinBox>(); TargetSlotsInput->SetMinValue(1); TargetSlotsInput->SetMaxValue(999); TargetSlotsInput->SetMinSliderValue(1); TargetSlotsInput->SetMaxSliderValue(999); TargetSlotsInput->SetDelta(1); TargetSlotsInput->SetAlwaysUsesDeltaSnap(true); TargetSlotsInput->SetValue(1);
    AddRowItem(StationRow,TargetSlotsInput); ApplySlotsButton=MakeButton(WidgetTree,TEXT("Apply target slots")); AddRowItem(StationRow,ApplySlotsButton);
    RemoveStationButton=MakeButton(WidgetTree,TEXT("Remove station")); AddRowItem(StationRow,RemoveStationButton);

    EditorStatusText=MakeText(WidgetTree,TEXT("Ready."),14,FLinearColor(0.7f,0.8f,0.85f)); Editor->AddChildToVerticalBox(EditorStatusText)->SetPadding(FMargin(5));
}

void UDynamicTimeTableWidget::BuildSettingsPanel(UVerticalBox* Page)
{
    UBorder* Border = WidgetTree->ConstructWidget<UBorder>();
    Border->SetBrushColor(FLinearColor(0.08f,0.12f,0.15f,1)); Border->SetPadding(FMargin(10));
    Page->AddChildToVerticalBox(Border)->SetPadding(FMargin(5,5,5,8));
    UVerticalBox* Box = WidgetTree->ConstructWidget<UVerticalBox>(); Border->SetContent(Box);
    Box->AddChildToVerticalBox(MakeText(WidgetTree,TEXT("Settings"),19,FLinearColor(0.2f,0.8f,1)));
    Box->AddChildToVerticalBox(MakeText(WidgetTree,TEXT("Saved with the world; the server's values apply. Save writes them, Defaults restores the shipped values."),13,FLinearColor(0.6f,0.7f,0.75f)))->SetPadding(FMargin(0,0,0,6));

    auto Spin = [&](const TCHAR* Label, const TCHAR* Hint, float Min, float Max, float Delta) -> USpinBox*
    {
        UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(); Box->AddChildToVerticalBox(Row)->SetPadding(FMargin(0,2));
        UVerticalBox* Text = WidgetTree->ConstructWidget<UVerticalBox>();
        Text->AddChildToVerticalBox(MakeText(WidgetTree,Label,15,FLinearColor::White));
        Text->AddChildToVerticalBox(MakeText(WidgetTree,Hint,12,FLinearColor(0.6f,0.7f,0.75f)));
        AddRowItem(Row,Text,5,true);
        USpinBox* S = WidgetTree->ConstructWidget<USpinBox>();
        S->SetMinValue(Min); S->SetMaxValue(Max); S->SetMinSliderValue(Min); S->SetMaxSliderValue(Max);
        S->SetDelta(Delta); S->SetAlwaysUsesDeltaSnap(true); S->SetMinFractionalDigits(Delta < 1.0f ? 1 : 0); S->SetMaxFractionalDigits(Delta < 1.0f ? 2 : 0);
        USizeBox* Size = WidgetTree->ConstructWidget<USizeBox>(); Size->SetWidthOverride(140); Size->SetContent(S);
        AddRowItem(Row,Size);
        return S;
    };
    auto Check = [&](const TCHAR* Label, const TCHAR* Hint) -> UCheckBox*
    {
        UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>(); Box->AddChildToVerticalBox(Row)->SetPadding(FMargin(0,2));
        UVerticalBox* Text = WidgetTree->ConstructWidget<UVerticalBox>();
        Text->AddChildToVerticalBox(MakeText(WidgetTree,Label,15,FLinearColor::White));
        Text->AddChildToVerticalBox(MakeText(WidgetTree,Hint,12,FLinearColor(0.6f,0.7f,0.75f)));
        AddRowItem(Row,Text,5,true);
        UCheckBox* C = WidgetTree->ConstructWidget<UCheckBox>();
        AddRowItem(Row,C,5);
        return C;
    };
    SettingLeftoverHold = Spin(TEXT("Leftover hold, seconds"), TEXT("How long a truck stays at the last stop of its lap unloading what the threshold left on board. 0 turns it off."), 0, 600, 5);
    SettingTopUpInterval = Spin(TEXT("Top-up interval, seconds"), TEXT("How often a held truck tries to unload again (requester or depot)."), 1, 30, 1);
    SettingUnloadAtDepot = Check(TEXT("Unload at the depot"), TEXT("A parked truck empties into its depot when the depot is in UNLOAD mode."));
    SettingSkipProducer = Check(TEXT("Skip the producer when cargo covers the lap"), TEXT("A pool truck whose cargo covers every planned deficit goes straight to the requesters."));
    SettingDispatchInterval = Spin(TEXT("Pool dispatch interval, seconds"), TEXT("How often parked trucks are matched to open requests."), 1, 60, 1);
    SettingMaxRequesters = Spin(TEXT("Requesters per lap"), TEXT("Upper bound on stops planned into one lap."), 1, 8, 1);
    SettingClaimTimeout = Spin(TEXT("Requester claim timeout, seconds"), TEXT("An unserved claim on a requester is dropped after this long."), 30, 3600, 30);
    SettingIdleRecheck = Spin(TEXT("Idle recheck, seconds"), TEXT("How often a dispatch truck idling on a one-stop route looks for requests."), 1, 120, 1);
    SettingFuelCritical = Spin(TEXT("Fuel: borrow a stop below, laps"), TEXT("A truck adds a fuel stop when its tank holds less than this many estimated laps."), 1, 10, 0.1f);
    SettingFuelSkip = Spin(TEXT("Fuel: skip own stop above, laps"), TEXT("A truck leaves its own (F) out when its tank holds more than this many laps. Kept above the borrow margin."), 1.1f, 10, 0.1f);

    UHorizontalBox* Buttons = WidgetTree->ConstructWidget<UHorizontalBox>(); Box->AddChildToVerticalBox(Buttons)->SetPadding(FMargin(0,8,0,0));
    SettingsSaveButton = MakeButton(WidgetTree,TEXT("Save")); AddRowItem(Buttons,SettingsSaveButton);
    SettingsDefaultsButton = MakeButton(WidgetTree,TEXT("Defaults")); AddRowItem(Buttons,SettingsDefaultsButton);
    SettingsStatusText = MakeText(WidgetTree,TEXT("Ready."),14,FLinearColor(0.7f,0.8f,0.85f)); Box->AddChildToVerticalBox(SettingsStatusText)->SetPadding(FMargin(5));
}

FDTTSettings UDynamicTimeTableWidget::ViewSettings() const
{
    if (IsRemoteClient() && RemoteCallObject) return RemoteCallObject->GetSnapshot().Settings;
    if (auto* S = GetWorld() ? GetWorld()->GetSubsystem<UDynamicTimeTableSubsystem>() : nullptr) return S->GetSettings();
    return FDTTSettings();
}

void UDynamicTimeTableWidget::RefreshSettingsControls(bool bForce)
{
    if (!SettingLeftoverHold || (bSettingsEdited && !bForce)) return;
    const FDTTSettings V = ViewSettings();
    bRefreshingControls = true;
    SettingLeftoverHold->SetValue(V.LeftoverHoldSeconds);
    SettingTopUpInterval->SetValue(V.TopUpIntervalSeconds);
    SettingUnloadAtDepot->SetIsChecked(V.bUnloadAtDepot);
    SettingSkipProducer->SetIsChecked(V.bSkipProducerWhenCargoCovers);
    SettingDispatchInterval->SetValue(V.PoolDispatchIntervalSeconds);
    SettingMaxRequesters->SetValue(float(V.MaxRequestersPerLap));
    SettingClaimTimeout->SetValue(V.RequesterClaimTimeoutSeconds);
    SettingIdleRecheck->SetValue(V.IdleDispatchRecheckSeconds);
    SettingFuelCritical->SetValue(V.FuelCriticalMarginLaps);
    SettingFuelSkip->SetValue(V.FuelSkipMarginLaps);
    bRefreshingControls = false;
    bSettingsEdited = false;
}

FDTTSettings UDynamicTimeTableWidget::ReadSettingsControls() const
{
    FDTTSettings V = ViewSettings();
    if (!SettingLeftoverHold) return V;
    V.LeftoverHoldSeconds = SettingLeftoverHold->GetValue();
    V.TopUpIntervalSeconds = SettingTopUpInterval->GetValue();
    V.bUnloadAtDepot = SettingUnloadAtDepot->IsChecked();
    V.bSkipProducerWhenCargoCovers = SettingSkipProducer->IsChecked();
    V.PoolDispatchIntervalSeconds = SettingDispatchInterval->GetValue();
    V.MaxRequestersPerLap = FMath::RoundToInt(SettingMaxRequesters->GetValue());
    V.RequesterClaimTimeoutSeconds = SettingClaimTimeout->GetValue();
    V.IdleDispatchRecheckSeconds = SettingIdleRecheck->GetValue();
    V.FuelCriticalMarginLaps = SettingFuelCritical->GetValue();
    V.FuelSkipMarginLaps = SettingFuelSkip->GetValue();
    V.Sanitize();
    return V;
}

void UDynamicTimeTableWidget::HandleSettingSpinChanged(float) { if (!bRefreshingControls) bSettingsEdited = true; }
void UDynamicTimeTableWidget::HandleSettingCheckChanged(bool) { if (!bRefreshingControls) bSettingsEdited = true; }
void UDynamicTimeTableWidget::HandleSettingsTabClicked() { SetActiveTab(3); RefreshSettingsControls(true); }
void UDynamicTimeTableWidget::HandleSettingsSaveClicked()
{
    const FDTTSettings V = ReadSettingsControls();
    if (IsRemoteClient())
    {
        if (RemoteCallObject) RemoteCallObject->RequestSetSettings(V); else SetEditorStatus(TEXT("Server connection unavailable."),true);
        return;
    }
    if (auto* S = GetWorld()->GetSubsystem<UDynamicTimeTableSubsystem>())
    {
        FString Message;
        const bool bOk = S->ApplySettings(V, TEXT("window"), &Message);
        SetEditorStatus(Message, !bOk);
        RefreshSettingsControls(true);
    }
}
void UDynamicTimeTableWidget::HandleSettingsDefaultsClicked()
{
    const FDTTSettings Defaults;
    bRefreshingControls = true;
    SettingLeftoverHold->SetValue(Defaults.LeftoverHoldSeconds);
    SettingTopUpInterval->SetValue(Defaults.TopUpIntervalSeconds);
    SettingUnloadAtDepot->SetIsChecked(Defaults.bUnloadAtDepot);
    SettingSkipProducer->SetIsChecked(Defaults.bSkipProducerWhenCargoCovers);
    SettingDispatchInterval->SetValue(Defaults.PoolDispatchIntervalSeconds);
    SettingMaxRequesters->SetValue(float(Defaults.MaxRequestersPerLap));
    SettingClaimTimeout->SetValue(Defaults.RequesterClaimTimeoutSeconds);
    SettingIdleRecheck->SetValue(Defaults.IdleDispatchRecheckSeconds);
    SettingFuelCritical->SetValue(Defaults.FuelCriticalMarginLaps);
    SettingFuelSkip->SetValue(Defaults.FuelSkipMarginLaps);
    bRefreshingControls = false;
    bSettingsEdited = true;
    SetEditorStatus(TEXT("Defaults loaded into the controls; Save to apply."), false);
}

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
bool UDynamicTimeTableWidget::RuntimeLayoutMatches() const
{
    const TArray<FDTTStationGroup>& Groups = ViewGroups();
    if (RuntimeGroupIds.Num() != Groups.Num() ||
        RuntimeGroupTexts.Num() != Groups.Num() ||
        RuntimeGroupBorders.Num() != Groups.Num() ||
        RuntimeStationCounts.Num() != Groups.Num())
        return false;

    int32 FlatStationIndex = 0;
    for (int32 GroupIndex = 0; GroupIndex < Groups.Num(); ++GroupIndex)
    {
        const FDTTStationGroup& Group = Groups[GroupIndex];
        if (RuntimeGroupIds[GroupIndex] != Group.GroupId ||
            RuntimeStationCounts[GroupIndex] != Group.Stations.Num())
            return false;

        for (const FDTTStationEntry& Entry : Group.Stations)
        {
            if (!RuntimeStationNames.IsValidIndex(FlatStationIndex) ||
                RuntimeStationNames[FlatStationIndex] != Entry.DisplayName)
                return false;
            ++FlatStationIndex;
        }
    }

    return FlatStationIndex == RuntimeStationNames.Num() &&
        FlatStationIndex == RuntimeStationTexts.Num();
}
void UDynamicTimeTableWidget::RebuildRuntimeDisplay()
{
    if (!GroupScrollBox) return;

    GroupScrollBox->ClearChildren();
    RuntimeGroupBorders.Reset();
    RuntimeGroupTexts.Reset();
    RuntimeStationTexts.Reset();
    RuntimeGroupIds.Reset();
    RuntimeStationCounts.Reset();
    RuntimeStationNames.Reset();

    const TArray<FDTTStationGroup>& Groups = ViewGroups();
    SummaryText->SetText(FText::FromString(FString::Printf(
        TEXT("%d group(s) - %s"),
        Groups.Num(),
        IsRemoteClient() ? TEXT("server snapshot") : TEXT("live booking status"))));

    for (const FDTTStationGroup& Group : Groups)
    {
        RuntimeGroupIds.Add(Group.GroupId);
        RuntimeStationCounts.Add(Group.Stations.Num());

        UBorder* Border = WidgetTree->ConstructWidget<UBorder>();
        Border->SetPadding(FMargin(14));
        GroupScrollBox->AddChild(Border);
        RuntimeGroupBorders.Add(Border);

        UVerticalBox* Panel = WidgetTree->ConstructWidget<UVerticalBox>();
        Border->SetContent(Panel);

        UTextBlock* GroupText = MakeText(WidgetTree, Group.GroupName, 21, FLinearColor(0.2f,0.8f,1));
        Panel->AddChildToVerticalBox(GroupText);
        RuntimeGroupTexts.Add(GroupText);

        for (const FDTTStationEntry& Entry : Group.Stations)
        {
            UTextBlock* StationText = MakeText(WidgetTree,
                UDynamicTimeTableSubsystem::StripStationTag(Entry.DisplayName), 16, FLinearColor::White);
            Panel->AddChildToVerticalBox(StationText);
            RuntimeStationTexts.Add(StationText);
            RuntimeStationNames.Add(Entry.DisplayName);
        }
    }

    UpdateRuntimeDisplayInPlace();
}
void UDynamicTimeTableWidget::UpdateRuntimeDisplayInPlace()
{
    const TArray<FDTTStationGroup>& Groups = ViewGroups();
    SummaryText->SetText(FText::FromString(FString::Printf(
        TEXT("%d group(s) - %s"),
        Groups.Num(),
        IsRemoteClient() ? TEXT("server snapshot") : TEXT("live booking status"))));

    int32 FlatStationIndex = 0;
    for (int32 GroupIndex = 0; GroupIndex < Groups.Num(); ++GroupIndex)
    {
        const FDTTStationGroup& Group = Groups[GroupIndex];
        TArray<FDTTStationRuntimeLoad> Loads;

        if (IsRemoteClient() && RemoteCallObject)
        {
            for (const FDTTStationRuntimeLoad& Load : RemoteCallObject->GetSnapshot().RuntimeLoads)
            {
                if (Group.Stations.ContainsByPredicate([&](const FDTTStationEntry& Entry)
                    { return Entry.DisplayName == Load.StationName; }))
                    Loads.Add(Load);
            }
        }
        else if (UDynamicTimeTableSubsystem* Subsystem = GetWorld()->GetSubsystem<UDynamicTimeTableSubsystem>())
        {
            Subsystem->GetGroupRuntimeLoad(Group.GroupId, Loads);
        }

        int32 Assigned = 0;
        int32 Target = 0;
        for (const FDTTStationRuntimeLoad& Load : Loads)
        {
            Assigned += Load.AssignedTrains;
            Target += Load.TargetSlots;
        }

        const bool bOverloaded = Assigned > Target;
        RuntimeGroupBorders[GroupIndex]->SetBrushColor(
            bOverloaded
                ? FLinearColor(0.25f,0.035f,0.025f,0.95f)
                : FLinearColor(0.06f,0.10f,0.13f,0.95f));
        RuntimeGroupTexts[GroupIndex]->SetText(FText::FromString(FString::Printf(
            TEXT("%s  [%s]    %d/%d%s"),
            *Group.GroupName,
            Group.Kind == EDTTGroupKind::Truck ? TEXT("Trucks") : TEXT("Trains"),
            Assigned,
            Target,
            bOverloaded ? TEXT("  OVERLOAD") : TEXT(""))));
        RuntimeGroupTexts[GroupIndex]->SetColorAndOpacity(FSlateColor(
            bOverloaded ? FLinearColor::Red : FLinearColor(0.2f,0.8f,1)));

        for (const FDTTStationEntry& Entry : Group.Stations)
        {
            const FDTTStationRuntimeLoad* Load = Loads.FindByPredicate(
                [&](const FDTTStationRuntimeLoad& Candidate)
                { return Candidate.StationName == Entry.DisplayName; });

            const int32 StationAssigned = Load ? Load->AssignedTrains : 0;
            const int32 StationTarget = Load ? Load->TargetSlots : FMath::Max(1, Entry.TargetSlots);
            const bool bStationOverloaded = Load ? Load->bOverloaded : StationAssigned > StationTarget;
            // The station is still handed out as a target - this only tells the player why their
            // trucks report StationUnreachable.
            const bool bStationUnreachable = Load && Load->bUnreachable;

            UTextBlock* StationText = RuntimeStationTexts[FlatStationIndex++];
            StationText->SetText(FText::FromString(FString::Printf(
                TEXT("    %s     booked %d / target %d%s%s"),
                *UDynamicTimeTableSubsystem::StripStationTag(Entry.DisplayName),
                StationAssigned,
                StationTarget,
                bStationOverloaded ? TEXT("  OVERLOAD") : TEXT(""),
                bStationUnreachable ? TEXT("  NOT ON THIS ROAD NETWORK") : TEXT(""))));
            StationText->SetColorAndOpacity(FSlateColor(
                bStationOverloaded ? FLinearColor::Red
                    : bStationUnreachable ? FLinearColor(1.0f,0.55f,0.1f)
                    : FLinearColor::White));
        }
    }
}
void UDynamicTimeTableWidget::RefreshEditorControls(){if(bRefreshingControls)return;bRefreshingControls=true;const auto&Groups=ViewGroups();if(!SelectedGroupId.IsValid()||!Groups.ContainsByPredicate([&](const FDTTStationGroup&G){return G.GroupId==SelectedGroupId;}))SelectedGroupId=Groups.Num()?Groups[0].GroupId:FGuid();GroupSelector->ClearOptions();GroupOptionIds.Reset();const FDTTStationGroup*Selected=nullptr;for(const auto&G:Groups){GroupSelector->AddOption(G.GroupName);GroupOptionIds.Add(G.GroupId);if(G.GroupId==SelectedGroupId)Selected=&G;}if(Selected){GroupSelector->SetSelectedOption(Selected->GroupName);GroupNameInput->SetText(FText::FromString(Selected->GroupName));}else GroupNameInput->SetText(FText::GetEmpty());GroupStationSelector->ClearOptions();if(Selected)for(const auto&E:Selected->Stations)GroupStationSelector->AddOption(E.DisplayName);if(GroupStationSelector->GetOptionCount()>0)GroupStationSelector->SetSelectedIndex(0);const EDTTGroupKind Kind=Selected?Selected->Kind:EDTTGroupKind::Train;const bool bTruck=Kind==EDTTGroupKind::Truck;TArray<FString>All;if(IsRemoteClient()&&RemoteCallObject)All=bTruck?RemoteCallObject->GetSnapshot().AvailableTruckStations:RemoteCallObject->GetSnapshot().AvailableStations;else if(auto*S=GetWorld()->GetSubsystem<UDynamicTimeTableSubsystem>()){if(bTruck)S->GetAllDockingStationNames(All);else S->GetAllVanillaStationNames(All);}AvailableStationSelector->ClearOptions();// A rail station and a docking station may share a name, so only groups of the same kind claim it.
for(const FString&N:All)if(!Groups.ContainsByPredicate([&](const FDTTStationGroup&G){return G.Kind==Kind&&G.Stations.ContainsByPredicate([&](const FDTTStationEntry&E){return E.DisplayName==N;});}))AvailableStationSelector->AddOption(N);if(AvailableStationSelector->GetOptionCount()>0)AvailableStationSelector->SetSelectedIndex(0);bRefreshingControls=false;HandleGroupStationSelectionChanged(GroupStationSelector->GetSelectedOption(),ESelectInfo::Direct);}
void UDynamicTimeTableWidget::SetEditorStatus(const FString&M,bool E){const FSlateColor C(E?FLinearColor::Red:FLinearColor(0.3f,1,0.5f));EditorStatusText->SetText(FText::FromString(M));EditorStatusText->SetColorAndOpacity(C);if(SettingsStatusText){SettingsStatusText->SetText(FText::FromString(M));SettingsStatusText->SetColorAndOpacity(C);}}FString UDynamicTimeTableWidget::GetSelectedGroupStationName()const{return GroupStationSelector?GroupStationSelector->GetSelectedOption():FString();}
void UDynamicTimeTableWidget::HandleGroupSelectionChanged(FString Item,ESelectInfo::Type){if(bRefreshingControls)return;int32 I=GroupSelector->FindOptionIndex(Item);if(GroupOptionIds.IsValidIndex(I)){SelectedGroupId=GroupOptionIds[I];RefreshEditorControls();}}
void UDynamicTimeTableWidget::HandleGroupStationSelectionChanged(FString Item,ESelectInfo::Type){if(Item.IsEmpty())return;if(const auto*G=ViewGroups().FindByPredicate([&](const FDTTStationGroup&X){return X.GroupId==SelectedGroupId;}))if(const auto*E=G->Stations.FindByPredicate([&](const FDTTStationEntry&X){return X.DisplayName==Item;}))TargetSlotsInput->SetValue(E->TargetSlots);}
void UDynamicTimeTableWidget::HandleCreateGroupClicked(){FString N=NewGroupNameInput->GetText().ToString().TrimStartAndEnd();if(N.IsEmpty()){SetEditorStatus(TEXT("Enter a group name."),true);return;}const EDTTGroupKind Kind=(NewGroupKindSelector&&NewGroupKindSelector->GetSelectedIndex()==1)?EDTTGroupKind::Truck:EDTTGroupKind::Train;if(IsRemoteClient()){if(RemoteCallObject)RemoteCallObject->RequestCreateGroup(N,Kind);else SetEditorStatus(TEXT("Server connection unavailable."),true);}else if(auto*S=GetWorld()->GetSubsystem<UDynamicTimeTableSubsystem>()){SelectedGroupId=S->CreateGroup(N,Kind);RefreshData();}NewGroupNameInput->SetText(FText::GetEmpty());}
void UDynamicTimeTableWidget::HandleRenameGroupClicked(){FString N=GroupNameInput->GetText().ToString().TrimStartAndEnd();if(IsRemoteClient()){if(RemoteCallObject)RemoteCallObject->RequestRenameGroup(SelectedGroupId,N);}else if(auto*S=GetWorld()->GetSubsystem<UDynamicTimeTableSubsystem>())SetEditorStatus(S->RenameGroup(SelectedGroupId,N)?TEXT("Group renamed."):TEXT("Rename failed."));}
void UDynamicTimeTableWidget::HandleDeleteGroupClicked(){if(IsRemoteClient()){if(RemoteCallObject)RemoteCallObject->RequestDeleteGroup(SelectedGroupId);}else if(auto*S=GetWorld()->GetSubsystem<UDynamicTimeTableSubsystem>()){S->RemoveGroup(SelectedGroupId);SelectedGroupId=FGuid();RefreshData();}}
void UDynamicTimeTableWidget::HandleAddStationClicked(){FString N=AvailableStationSelector->GetSelectedOption();if(IsRemoteClient()){if(RemoteCallObject)RemoteCallObject->RequestAddStation(SelectedGroupId,N);}else if(auto*S=GetWorld()->GetSubsystem<UDynamicTimeTableSubsystem>())S->AddStationByName(SelectedGroupId,N,1);}
void UDynamicTimeTableWidget::HandleApplySlotsClicked(){int32 Slots=FMath::Clamp(FMath::RoundToInt(TargetSlotsInput->GetValue()),1,999);if(IsRemoteClient()){if(RemoteCallObject)RemoteCallObject->RequestSetTargetSlots(SelectedGroupId,GetSelectedGroupStationName(),Slots);}else if(auto*S=GetWorld()->GetSubsystem<UDynamicTimeTableSubsystem>())S->SetTargetSlots(SelectedGroupId,GetSelectedGroupStationName(),Slots);}
void UDynamicTimeTableWidget::HandleRemoveStationClicked(){if(IsRemoteClient()){if(RemoteCallObject)RemoteCallObject->RequestRemoveStation(SelectedGroupId,GetSelectedGroupStationName());}else if(auto*S=GetWorld()->GetSubsystem<UDynamicTimeTableSubsystem>())S->RemoveStationByName(SelectedGroupId,GetSelectedGroupStationName());}
void UDynamicTimeTableWidget::HandleRecalculateClicked(){if(IsRemoteClient()){if(RemoteCallObject)RemoteCallObject->RequestRecalculate();}else if(auto*S=GetWorld()->GetSubsystem<UDynamicTimeTableSubsystem>())S->RecalculateBookingsFromVanilla();}
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
    // A remote client shows the last snapshot of this section until the next one arrives; ask now
    // rather than at the next 1.5 s tick.
    if (IsRemoteClient() && RemoteCallObject) RemoteCallObject->RequestSnapshot(GetRequestedSections());
    RefreshRuntimeDisplay();
}

void UDynamicTimeTableWidget::HandleGroupsTabClicked() { SetActiveTab(0); }
void UDynamicTimeTableWidget::HandleDispatchTabClicked() { SetActiveTab(1); }
void UDynamicTimeTableWidget::HandleTrucksTabClicked() { SetActiveTab(2); }

void UDynamicTimeTableWidget::RefreshRowList(
    UScrollBox* Box, TArray<TObjectPtr<UTextBlock>>& Texts, TArray<FString>& Keys,
    const TArray<FString>& NewKeys, const TArray<FString>& Lines, const TArray<FLinearColor>& Colors)
{
    if (!Box) return;
    if (Keys != NewKeys || Texts.Num() != NewKeys.Num())
    {
        Box->ClearChildren();
        Texts.Reset();
        Keys = NewKeys;
        for (int32 Index = 0; Index < NewKeys.Num(); ++Index)
        {
            UTextBlock* Text = MakeText(WidgetTree, FString(), 15, FLinearColor::White);
            Text->SetAutoWrapText(true);
            Box->AddChild(Text);
            Texts.Add(Text);
        }
    }
    for (int32 Index = 0; Index < Texts.Num() && Index < Lines.Num(); ++Index)
    {
        Texts[Index]->SetText(FText::FromString(Lines[Index]));
        Texts[Index]->SetColorAndOpacity(FSlateColor(Colors.IsValidIndex(Index) ? Colors[Index] : FLinearColor::White));
    }
}

void UDynamicTimeTableWidget::RefreshDispatchDisplay()
{
    TArray<FDTTDispatchRow> Rows;
    if (IsRemoteClient())
    {
        if (RemoteCallObject) Rows = RemoteCallObject->GetSnapshot().DispatchRows;
    }
    else if (UDynamicTimeTableSubsystem* Subsystem = GetWorld()->GetSubsystem<UDynamicTimeTableSubsystem>())
    {
        Subsystem->BuildDispatchRows(Rows);
    }

    TArray<FString> Keys, Lines;
    TArray<FLinearColor> Colors;
    int32 Open = 0, Depots = 0;
    for (const FDTTDispatchRow& Row : Rows)
    {
        Keys.Add(FString::Printf(TEXT("%s|%s|%s"), Row.bDepot ? TEXT("D") : Row.bFuelStop ? TEXT("F") : *Row.ResourceId,
            *Row.DockingPathNodeGuid.ToString(), *Row.StationName));
        Lines.Add(UDynamicTimeTableSubsystem::FormatDispatchRow(Row));
        FLinearColor Color = FLinearColor::White;
        if (Row.bDepot) { Color = FLinearColor(0.6f, 0.7f, 0.85f); ++Depots; }
        else if (Row.bFuelStop) Color = FLinearColor(1.0f, 0.75f, 0.35f);
        else if (!Row.bModeMatches) Color = FLinearColor::Red;
        else if (Row.Role == EDTTDispatchRole::Producer) Color = FLinearColor(0.2f, 0.8f, 1.0f);
        else if (Row.bOpen) { Color = Row.ClaimOwner.IsEmpty() ? FLinearColor(0.3f, 1.0f, 0.5f) : FLinearColor(1.0f, 0.85f, 0.3f); ++Open; }
        Colors.Add(Color);
    }
    if (Rows.Num() == 0)
    {
        Keys.Add(TEXT("empty"));
        Lines.Add(TEXT("No dispatch tags. Use (Resource) on LOAD/UNLOAD stations, (Resource-N) to request only with more than N stacks free, (F) on a fuel stop, (D) on a depot."));
        Colors.Add(FLinearColor(0.7f, 0.8f, 0.85f));
    }
    if (SummaryText)
    {
        SummaryText->SetText(FText::FromString(FString::Printf(TEXT("%d dispatch row(s), %d open, %d depot(s) - %s"),
            Rows.Num(), Open, Depots, IsRemoteClient() ? TEXT("server snapshot") : TEXT("live"))));
    }
    RefreshRowList(DispatchScrollBox, DispatchRowTexts, DispatchRowKeys, Keys, Lines, Colors);
}

void UDynamicTimeTableWidget::RefreshTruckDisplay()
{
    TArray<FDTTTruckRow> Rows;
    if (IsRemoteClient())
    {
        if (RemoteCallObject) Rows = RemoteCallObject->GetSnapshot().TruckRows;
    }
    else if (UDynamicTimeTableSubsystem* Subsystem = GetWorld()->GetSubsystem<UDynamicTimeTableSubsystem>())
    {
        Subsystem->BuildTruckRows(Rows);
    }

    TArray<FString> Keys, Lines;
    TArray<FLinearColor> Colors;
    int32 Pool = 0, Parked = 0;
    for (const FDTTTruckRow& Row : Rows)
    {
        Keys.Add(FString::Printf(TEXT("%s|%s"), Row.bPool ? TEXT("P") : TEXT("L"), *Row.Name));
        Lines.Add(UDynamicTimeTableSubsystem::FormatTruckRow(Row));
        FLinearColor Color = FLinearColor::White;
        if (!Row.bAutopilot) Color = FLinearColor(1.0f, 0.55f, 0.1f);
        else if (Row.bPool)
        {
            switch (Row.Phase)
            {
            case EDTTTruckPhase::Parked: Color = FLinearColor(0.6f, 0.6f, 0.6f); break;
            case EDTTTruckPhase::Assigned: Color = FLinearColor(0.2f, 0.8f, 1.0f); break;
            case EDTTTruckPhase::Manual: Color = FLinearColor(1.0f, 0.55f, 0.1f); break;
            case EDTTTruckPhase::ToDepot: Color = FLinearColor(0.6f, 0.7f, 0.85f); break;
            default: break;
            }
        }
        if (Row.bPool) { ++Pool; if (Row.Phase == EDTTTruckPhase::Parked) ++Parked; }
        Colors.Add(Color);
    }
    if (Rows.Num() == 0)
    {
        Keys.Add(TEXT("empty"));
        Lines.Add(TEXT("No managed trucks. A truck whose first stop is a (Resource) LOAD station runs dispatch laps; one whose route contains a (D) depot joins the pool."));
        Colors.Add(FLinearColor(0.7f, 0.8f, 0.85f));
    }
    if (SummaryText)
    {
        SummaryText->SetText(FText::FromString(FString::Printf(TEXT("%d truck(s), %d in the pool, %d parked - %s"),
            Rows.Num(), Pool, Parked, IsRemoteClient() ? TEXT("server snapshot") : TEXT("live"))));
    }
    RefreshRowList(TruckScrollBox, TruckRowTexts, TruckRowKeys, Keys, Lines, Colors);
}
