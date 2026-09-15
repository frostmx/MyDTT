#include "DynamicTimeTableWidget.h"

#include "DynamicTimeTableSubsystem.h"
#include "DynamicTimeTableRemoteCallObject.h"
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
    for (USpinBox* Spin : { SettingSolidRequest.Get(), SettingFluidRequest.Get(), SettingRequestBuffer.Get(),
        SettingLeftoverHold.Get(), SettingTopUpInterval.Get(), SettingDispatchInterval.Get(), SettingMaxRequesters.Get(),
        SettingClaimTimeout.Get(), SettingOverdueMinutes.Get(), SettingIdleRecheck.Get(), SettingFuelCritical.Get(), SettingFuelSkip.Get() })
    {
        if (Spin) Spin->OnValueChanged.AddDynamic(this, &UDynamicTimeTableWidget::HandleSettingSpinChanged);
    }
    for (UCheckBox* Check : { SettingExactQuantities.Get(), SettingFluidFullLoad.Get(), SettingDrainLeftovers.Get(), SettingUnloadAtDepot.Get(), SettingSkipProducer.Get() })
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

    Pages->AddChild(BuildCardPage(DispatchScrollBox, DispatchFilter));
    DispatchFilter->OnTextChanged.AddDynamic(this, &UDynamicTimeTableWidget::HandleDispatchFilterChanged);
    Pages->AddChild(BuildCardPage(TruckScrollBox, TruckFilter));
    TruckFilter->OnTextChanged.AddDynamic(this, &UDynamicTimeTableWidget::HandleTruckFilterChanged);
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

UVerticalBox* UDynamicTimeTableWidget::BuildCardPage(TObjectPtr<UScrollBox>& OutScroll, TObjectPtr<UEditableTextBox>& OutFilter)
{
    UVerticalBox* Page = WidgetTree->ConstructWidget<UVerticalBox>();
    UHorizontalBox* FilterRow = WidgetTree->ConstructWidget<UHorizontalBox>();
    Page->AddChildToVerticalBox(FilterRow)->SetPadding(FMargin(4, 2, 4, 4));
    AddRowItem(FilterRow, MakeText(WidgetTree, TEXT("Filter"), 13, FLinearColor(0.6f, 0.68f, 0.74f)), 4);
    OutFilter = WidgetTree->ConstructWidget<UEditableTextBox>();
    OutFilter->SetHintText(FText::FromString(TEXT("name or resource, any case")));
    FSlateFontInfo FilterFont = OutFilter->WidgetStyle.TextStyle.Font;
    FilterFont.Size = 13;
    OutFilter->WidgetStyle.SetFont(FilterFont).SetPadding(FMargin(8, 4));
    AddRowItem(FilterRow, OutFilter, 4, true);
    OutScroll = WidgetTree->ConstructWidget<UScrollBox>();
    Page->AddChildToVerticalBox(OutScroll)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
    return Page;
}

void UDynamicTimeTableWidget::HandleDispatchFilterChanged(const FText&) { RefreshDispatchDisplay(); }
void UDynamicTimeTableWidget::HandleTruckFilterChanged(const FText&) { RefreshTruckDisplay(); }

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
    // Header and the Save/Defaults row stay put; the rows scroll between them. The border fills
    // the page and clips, so a long list can never paint over the window's footer again.
    UBorder* Border = WidgetTree->ConstructWidget<UBorder>();
    Border->SetBrushColor(FLinearColor(0.08f,0.12f,0.15f,1)); Border->SetPadding(FMargin(10));
    Border->SetClipping(EWidgetClipping::ClipToBounds);
    UVerticalBoxSlot* BorderSlot = Page->AddChildToVerticalBox(Border);
    BorderSlot->SetPadding(FMargin(5,5,5,8)); BorderSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
    UVerticalBox* Box = WidgetTree->ConstructWidget<UVerticalBox>(); Border->SetContent(Box);
    Box->AddChildToVerticalBox(MakeText(WidgetTree,TEXT("Settings"),19,FLinearColor(0.2f,0.8f,1)));
    Box->AddChildToVerticalBox(MakeText(WidgetTree,TEXT("Saved with the world; the server's values apply. Save writes them, Defaults restores the shipped values."),13,FLinearColor(0.6f,0.7f,0.75f)))->SetPadding(FMargin(0,0,0,4));
    UScrollBox* Rows = WidgetTree->ConstructWidget<UScrollBox>();
    UVerticalBoxSlot* RowsSlot = Box->AddChildToVerticalBox(Rows);
    RowsSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill)); RowsSlot->SetPadding(FMargin(0,2));

    auto Section = [&](const TCHAR* Title)
    {
        UTextBlock* T = MakeText(WidgetTree,Title,16,FLinearColor(0.2f,0.8f,1));
        Rows->AddChild(T);
        if (UScrollBoxSlot* S = Cast<UScrollBoxSlot>(T->Slot)) S->SetPadding(FMargin(0,8,0,2));
    };
    auto Row = [&](const TCHAR* Label, const TCHAR* Hint, UWidget* Control)
    {
        UHorizontalBox* R = WidgetTree->ConstructWidget<UHorizontalBox>();
        Rows->AddChild(R);
        if (UScrollBoxSlot* S = Cast<UScrollBoxSlot>(R->Slot)) S->SetPadding(FMargin(0,1,8,1));
        UVerticalBox* Text = WidgetTree->ConstructWidget<UVerticalBox>();
        Text->AddChildToVerticalBox(MakeText(WidgetTree,Label,14,FLinearColor::White));
        UTextBlock* H = MakeText(WidgetTree,Hint,11,FLinearColor(0.6f,0.7f,0.75f)); H->SetAutoWrapText(true);
        Text->AddChildToVerticalBox(H);
        AddRowItem(R,Text,3,true);
        AddRowItem(R,Control,3);
    };
    auto Spin = [&](const TCHAR* Label, const TCHAR* Hint, float Min, float Max, float Delta) -> USpinBox*
    {
        USpinBox* S = WidgetTree->ConstructWidget<USpinBox>();
        S->SetMinValue(Min); S->SetMaxValue(Max); S->SetMinSliderValue(Min); S->SetMaxSliderValue(Max);
        S->SetDelta(Delta); S->SetAlwaysUsesDeltaSnap(true); S->SetMinFractionalDigits(Delta < 1.0f ? 1 : 0); S->SetMaxFractionalDigits(Delta < 1.0f ? 2 : 0);
        USizeBox* Size = WidgetTree->ConstructWidget<USizeBox>(); Size->SetWidthOverride(120); Size->SetContent(S);
        Row(Label, Hint, Size);
        return S;
    };
    auto Check = [&](const TCHAR* Label, const TCHAR* Hint) -> UCheckBox*
    {
        UCheckBox* C = WidgetTree->ConstructWidget<UCheckBox>();
        Row(Label, Hint, C);
        return C;
    };
    Section(TEXT("Requests"));
    SettingSolidRequest = Spin(TEXT("Solid request limit, stacks"), TEXT("A solid station without a number in its tag is called while it holds fewer stacks than this. (Res-N) in the name overrides it."), 0, 200, 1);
    SettingFluidRequest = Spin(TEXT("Fluid request limit, stacks of 50 000 L"), TEXT("A fluid station without a number in its tag is called while its tank holds fewer stacks than this (64 = a full tank). (Res-N) in the name is a percent."), 0, 200, 1);
    SettingRequestBuffer = Spin(TEXT("Request buffer, stacks"), TEXT("A delivery fills a station up to its limit plus this many stacks, covering what is consumed while the truck is on its way. The fill target only applies with exact quantities; otherwise it sizes the lap."), 0, 64, 1);
    SettingFluidFullLoad = Check(TEXT("Fluid trucks always carry a full load"), TEXT("A fluid lap serves one station in either mode. With exact quantities the producer also fills the tank completely and the truck drains it there."));
    SettingMaxRequesters = Spin(TEXT("Requesters per lap"), TEXT("Upper bound on stops planned into one lap."), 1, 8, 1);
    SettingClaimTimeout = Spin(TEXT("Requester claim timeout, seconds"), TEXT("An unserved claim on a requester is dropped after this long."), 30, 3600, 30);
    SettingOverdueMinutes = Spin(TEXT("Overdue after, minutes"), TEXT("A request nobody has claimed for this long is shown in red in the Dispatch tab and logged once. 0 turns it off."), 0, 120, 1);
    Section(TEXT("Transfer"));
    SettingExactQuantities = Check(TEXT("Exact quantities"), TEXT("On: the loading station gives a lap only what its requesters lack, each requester takes only up to its target, the last one drains the rest. Off (default): a full load at the producer and everything that fits at each requester; a truck that empties early starts its next lap from where it is."));
    SettingDrainLeftovers = Check(TEXT("Drain leftovers at the last stop"), TEXT("The last requester of a lap takes everything it can during the unload, target or not, and with a leftover hold the truck stays and tops it up. Works with exact quantities on or off. Off: the truck keeps its leftovers for the next lap."));
    SettingLeftoverHold = Spin(TEXT("Leftover hold, seconds"), TEXT("Extra time the drained truck stays at that last stop, topping the station up as it consumes. 0 (default): it leaves right after the unload."), 0, 600, 5);
    SettingStayUntilEmpty = Check(TEXT("Stay at the requester until empty"), TEXT("Off (default): a lap visits its stops one after the other. On: the truck stays docked at every stop of its lap, topping it up as it consumes, until its cargo is gone - then it drives on. A stop that stops consuming keeps the truck."));
    SettingTopUpInterval = Spin(TEXT("Top-up interval, seconds"), TEXT("How often a held truck tries to unload again (requester hold or depot)."), 1, 30, 1);
    SettingUnloadAtDepot = Check(TEXT("Unload at the depot"), TEXT("A parked truck empties into its depot when the depot is in UNLOAD mode."));
    SettingSkipProducer = Check(TEXT("Skip the producer when cargo covers the lap"), TEXT("A pool truck whose cargo covers every planned deficit goes straight to the requesters."));
    Section(TEXT("Pool"));
    SettingDispatchInterval = Spin(TEXT("Pool dispatch interval, seconds"), TEXT("How often parked trucks are matched to open requests."), 1, 60, 1);
    SettingIdleRecheck = Spin(TEXT("Idle recheck, seconds"), TEXT("How often a dispatch truck idling on a one-stop route looks for requests."), 1, 120, 1);
    Section(TEXT("Fuel"));
    SettingFuelCritical = Spin(TEXT("Borrow a stop below, laps"), TEXT("A truck adds a fuel stop when its tank holds less than this many estimated laps."), 1, 10, 0.1f);
    SettingFuelSkip = Spin(TEXT("Skip own stop above, laps"), TEXT("A truck leaves its own (F) out when its tank holds more than this many laps. Kept above the borrow margin."), 1.1f, 10, 0.1f);
    SettingFuelLegMargin = Spin(TEXT("Leg guard margin, x"), TEXT("Before every leg the tank must hold this many times the estimated cost of it, or a fuel stop goes in first. The engine's estimate is unverified against real consumption, so 1.0 strands trucks; 2 is the shipped value."), 1.0f, 5, 0.1f);
    SettingFuelReach = Spin(TEXT("Detour reach, share of the tank"), TEXT("A borrowed (F) is only placed where the tank can reach it on at most this share of its current energy. Lower plans more cautiously, 1.0 spends the whole tank on getting there."), 0.3f, 1.0f, 0.05f);

    UHorizontalBox* Buttons = WidgetTree->ConstructWidget<UHorizontalBox>(); Box->AddChildToVerticalBox(Buttons)->SetPadding(FMargin(0,6,0,0));
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
    SettingExactQuantities->SetIsChecked(V.bExactQuantities);
    SettingSolidRequest->SetValue(float(V.SolidRequestStacks));
    SettingFluidRequest->SetValue(float(V.FluidRequestStacks));
    SettingRequestBuffer->SetValue(float(V.RequestBufferStacks));
    SettingFluidFullLoad->SetIsChecked(V.bFluidFullLoad);
    SettingDrainLeftovers->SetIsChecked(V.bDrainLeftovers);
    SettingLeftoverHold->SetValue(V.LeftoverHoldSeconds);
    SettingStayUntilEmpty->SetIsChecked(V.bStayUntilEmpty);
    SettingTopUpInterval->SetValue(V.TopUpIntervalSeconds);
    SettingUnloadAtDepot->SetIsChecked(V.bUnloadAtDepot);
    SettingSkipProducer->SetIsChecked(V.bSkipProducerWhenCargoCovers);
    SettingDispatchInterval->SetValue(V.PoolDispatchIntervalSeconds);
    SettingMaxRequesters->SetValue(float(V.MaxRequestersPerLap));
    SettingClaimTimeout->SetValue(V.RequesterClaimTimeoutSeconds);
    SettingOverdueMinutes->SetValue(float(V.OverdueRequestMinutes));
    SettingIdleRecheck->SetValue(V.IdleDispatchRecheckSeconds);
    SettingFuelCritical->SetValue(V.FuelCriticalMarginLaps);
    SettingFuelSkip->SetValue(V.FuelSkipMarginLaps);
    SettingFuelLegMargin->SetValue(V.FuelLegMarginFactor);
    SettingFuelReach->SetValue(V.FuelReachFraction);
    bRefreshingControls = false;
    bSettingsEdited = false;
}

FDTTSettings UDynamicTimeTableWidget::ReadSettingsControls() const
{
    FDTTSettings V = ViewSettings();
    if (!SettingLeftoverHold) return V;
    V.bExactQuantities = SettingExactQuantities->IsChecked();
    V.SolidRequestStacks = FMath::RoundToInt(SettingSolidRequest->GetValue());
    V.FluidRequestStacks = FMath::RoundToInt(SettingFluidRequest->GetValue());
    V.RequestBufferStacks = FMath::RoundToInt(SettingRequestBuffer->GetValue());
    V.bFluidFullLoad = SettingFluidFullLoad->IsChecked();
    V.bDrainLeftovers = SettingDrainLeftovers->IsChecked();
    V.LeftoverHoldSeconds = SettingLeftoverHold->GetValue();
    V.bStayUntilEmpty = SettingStayUntilEmpty->IsChecked();
    V.TopUpIntervalSeconds = SettingTopUpInterval->GetValue();
    V.bUnloadAtDepot = SettingUnloadAtDepot->IsChecked();
    V.bSkipProducerWhenCargoCovers = SettingSkipProducer->IsChecked();
    V.PoolDispatchIntervalSeconds = SettingDispatchInterval->GetValue();
    V.MaxRequestersPerLap = FMath::RoundToInt(SettingMaxRequesters->GetValue());
    V.RequesterClaimTimeoutSeconds = SettingClaimTimeout->GetValue();
    V.OverdueRequestMinutes = FMath::RoundToInt(SettingOverdueMinutes->GetValue());
    V.IdleDispatchRecheckSeconds = SettingIdleRecheck->GetValue();
    V.FuelCriticalMarginLaps = SettingFuelCritical->GetValue();
    V.FuelSkipMarginLaps = SettingFuelSkip->GetValue();
    V.FuelLegMarginFactor = SettingFuelLegMargin->GetValue();
    V.FuelReachFraction = SettingFuelReach->GetValue();
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
    SettingExactQuantities->SetIsChecked(Defaults.bExactQuantities);
    SettingSolidRequest->SetValue(float(Defaults.SolidRequestStacks));
    SettingFluidRequest->SetValue(float(Defaults.FluidRequestStacks));
    SettingRequestBuffer->SetValue(float(Defaults.RequestBufferStacks));
    SettingFluidFullLoad->SetIsChecked(Defaults.bFluidFullLoad);
    SettingDrainLeftovers->SetIsChecked(Defaults.bDrainLeftovers);
    SettingLeftoverHold->SetValue(Defaults.LeftoverHoldSeconds);
    SettingTopUpInterval->SetValue(Defaults.TopUpIntervalSeconds);
    SettingUnloadAtDepot->SetIsChecked(Defaults.bUnloadAtDepot);
    SettingSkipProducer->SetIsChecked(Defaults.bSkipProducerWhenCargoCovers);
    SettingDispatchInterval->SetValue(Defaults.PoolDispatchIntervalSeconds);
    SettingMaxRequesters->SetValue(float(Defaults.MaxRequestersPerLap));
    SettingClaimTimeout->SetValue(Defaults.RequesterClaimTimeoutSeconds);
    SettingOverdueMinutes->SetValue(float(Defaults.OverdueRequestMinutes));
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

// A card is built once and then updated in place on every refresh: the sink hands out the card's
// stored widgets in creation order, so the walk that fills a card must produce the same shape
// for a build and for an update. The card key covers what decides the widget count; a key
// change rebuilds that one card, the rest of the page stays as it is (no flicker, no scroll
// jump). Text that is empty collapses, so optional lines are always emitted.
struct FDTTCardSink
{
    UWidgetTree* Tree = nullptr;
    bool bBuild = true;
    FDTTCardWidgets& Card;
    int32 TextIndex = 0, BorderIndex = 0, BarIndex = 0, ImageIndex = 0;

    FDTTCardSink(UWidgetTree* InTree, bool bInBuild, FDTTCardWidgets& InCard) : Tree(InTree), bBuild(bInBuild), Card(InCard) {}

    /** True when an update walked past what the build stored: the shape changed without the key noticing. */
    bool Overran() const { return TextIndex != Card.Texts.Num() || BorderIndex != Card.Borders.Num() || BarIndex != Card.Bars.Num() || ImageIndex != Card.Images.Num(); }

    template <typename TSlot>
    static void Pad(UWidget* Widget, const FMargin& Margin)
    {
        if (Widget) if (TSlot* S = Cast<TSlot>(Widget->Slot)) S->SetPadding(Margin);
    }

    UVerticalBox* VBox(UPanelWidget* Parent, const FMargin& Margin = FMargin(0))
    {
        if (!bBuild) return nullptr;
        UVerticalBox* Box = Tree->ConstructWidget<UVerticalBox>();
        if (Parent) { Parent->AddChild(Box); Pad<UVerticalBoxSlot>(Box, Margin); }
        return Box;
    }

    UHorizontalBox* HBox(UPanelWidget* Parent, const FMargin& Margin = FMargin(0))
    {
        if (!bBuild) return nullptr;
        UHorizontalBox* Box = Tree->ConstructWidget<UHorizontalBox>();
        if (Parent) { Parent->AddChild(Box); Pad<UVerticalBoxSlot>(Box, Margin); }
        return Box;
    }

    /** A column of a two-column body: fills its share, aligned to the top. */
    UVerticalBox* Column(UHorizontalBox* Row)
    {
        if (!bBuild) return nullptr;
        UVerticalBox* Box = Tree->ConstructWidget<UVerticalBox>();
        if (Row)
        {
            UHorizontalBoxSlot* S = Row->AddChildToHorizontalBox(Box);
            S->SetSize(FSlateChildSize(ESlateSizeRule::Fill)); S->SetPadding(FMargin(0, 0, 10, 0)); S->SetVerticalAlignment(VAlign_Top);
        }
        return Box;
    }

    UWrapBox* Wrap(UPanelWidget* Parent, const FMargin& Margin = FMargin(0))
    {
        if (!bBuild) return nullptr;
        UWrapBox* Box = Tree->ConstructWidget<UWrapBox>();
        Box->SetInnerSlotPadding(FVector2D(4, 2));
        if (Parent)
        {
            Parent->AddChild(Box); Pad<UVerticalBoxSlot>(Box, Margin);
            // In a row the wrap box must take the remaining width, or it wraps after every chip.
            if (UHorizontalBoxSlot* S = Cast<UHorizontalBoxSlot>(Box->Slot)) { S->SetPadding(Margin); S->SetSize(FSlateChildSize(ESlateSizeRule::Fill)); S->SetVerticalAlignment(VAlign_Center); }
        }
        return Box;
    }

    UTextBlock* Text(UPanelWidget* Parent, const FString& Value, int32 Size, const FLinearColor& Color,
        bool bWrap = false, const FMargin& Margin = FMargin(0), bool bFill = false)
    {
        UTextBlock* T = nullptr;
        if (bBuild)
        {
            T = MakeText(Tree, Value, Size, Color);
            if (bWrap) T->SetAutoWrapText(true);
            if (Parent)
            {
                Parent->AddChild(T);
                Pad<UVerticalBoxSlot>(T, Margin); Pad<UWrapBoxSlot>(T, Margin);
                if (UHorizontalBoxSlot* S = Cast<UHorizontalBoxSlot>(T->Slot))
                {
                    S->SetPadding(Margin); S->SetVerticalAlignment(VAlign_Center);
                    if (bFill) S->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
                }
            }
            Card.Texts.Add(T);
        }
        else if (Card.Texts.IsValidIndex(TextIndex))
        {
            T = Card.Texts[TextIndex].Get();
            if (T) { T->SetText(FText::FromString(Value)); T->SetColorAndOpacity(FSlateColor(Color)); }
        }
        if (T) T->SetVisibility(Value.IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::SelfHitTestInvisible);
        ++TextIndex;
        return T;
    }

    /** The card itself: a tinted border with a vertical body, stored as the card's root. Returns the body (null on an update). */
    UVerticalBox* Root(const FLinearColor& Tint)
    {
        UVerticalBox* Body = nullptr;
        if (bBuild)
        {
            UBorder* Border = Tree->ConstructWidget<UBorder>();
            Border->SetBrushColor(Tint); Border->SetPadding(FMargin(12, 8));
            Body = Tree->ConstructWidget<UVerticalBox>(); Border->SetContent(Body);
            Card.Root = Border;
            Card.Borders.Add(Border);
        }
        else if (Card.Borders.IsValidIndex(BorderIndex))
        {
            if (UBorder* Border = Card.Borders[BorderIndex].Get()) Border->SetBrushColor(Tint);
        }
        ++BorderIndex;
        return Body;
    }

    /** A small coloured label: "Pool · Parked", "Autopilot off". Hidden ones keep their place. */
    void Badge(UPanelWidget* Parent, const FString& Label, const FLinearColor& Background, bool bVisible = true, const FLinearColor& Foreground = FLinearColor::White)
    {
        UBorder* Border = nullptr;
        if (bBuild)
        {
            Border = Tree->ConstructWidget<UBorder>();
            Border->SetBrushColor(Background); Border->SetPadding(FMargin(7, 1));
            if (Parent)
            {
                Parent->AddChild(Border);
                if (UHorizontalBoxSlot* S = Cast<UHorizontalBoxSlot>(Border->Slot)) { S->SetPadding(FMargin(0, 0, 6, 0)); S->SetVerticalAlignment(VAlign_Center); }
            }
            Card.Borders.Add(Border);
        }
        else if (Card.Borders.IsValidIndex(BorderIndex))
        {
            Border = Card.Borders[BorderIndex].Get();
            if (Border) Border->SetBrushColor(Background);
        }
        ++BorderIndex;
        UTextBlock* T = Text(nullptr, Label, 12, Foreground);
        if (bBuild && Border && T) Border->SetContent(T);
        if (Border) Border->SetVisibility(bVisible ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
    }

    /** An item icon of Size points; collapsed when there is no texture, keeping its place. */
    void Icon(UPanelWidget* Parent, UTexture2D* Texture, float Size, const FMargin& Margin = FMargin(0, 0, 6, 0))
    {
        UImage* Image = nullptr;
        if (bBuild)
        {
            Image = Tree->ConstructWidget<UImage>();
            USizeBox* Box = Tree->ConstructWidget<USizeBox>();
            Box->SetWidthOverride(Size); Box->SetHeightOverride(Size); Box->SetContent(Image);
            if (Parent)
            {
                Parent->AddChild(Box);
                if (UHorizontalBoxSlot* S = Cast<UHorizontalBoxSlot>(Box->Slot)) { S->SetPadding(Margin); S->SetVerticalAlignment(VAlign_Center); }
                Pad<UVerticalBoxSlot>(Box, Margin);
            }
            Card.Images.Add(Image);
        }
        else if (Card.Images.IsValidIndex(ImageIndex))
        {
            Image = Card.Images[ImageIndex].Get();
        }
        ++ImageIndex;
        if (!Image) return;
        if (Texture) Image->SetBrushFromTexture(Texture, false);
        // The size box is what takes the space, so hide that.
        UWidget* Holder = Image->GetParent() ? static_cast<UWidget*>(Image->GetParent()) : static_cast<UWidget*>(Image);
        Holder->SetVisibility(Texture ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
    }

    /** A thin fill bar; Percent 0..1. */
    void Bar(UPanelWidget* Parent, float Percent, const FLinearColor& Color, const FMargin& Margin = FMargin(0, 3, 0, 2))
    {
        Percent = FMath::Clamp(Percent, 0.0f, 1.0f);
        if (bBuild)
        {
            UProgressBar* B = Tree->ConstructWidget<UProgressBar>();
            B->SetPercent(Percent); B->SetFillColorAndOpacity(Color);
            USizeBox* Size = Tree->ConstructWidget<USizeBox>(); Size->SetHeightOverride(7); Size->SetContent(B);
            if (Parent) { Parent->AddChild(Size); Pad<UVerticalBoxSlot>(Size, Margin); }
            Card.Bars.Add(B);
        }
        else if (Card.Bars.IsValidIndex(BarIndex))
        {
            if (UProgressBar* B = Card.Bars[BarIndex].Get()) { B->SetPercent(Percent); B->SetFillColorAndOpacity(Color); }
        }
        ++BarIndex;
    }
};

namespace
{
    const FLinearColor DTTCardTint(0.08f, 0.12f, 0.15f, 1.0f);
    const FLinearColor DTTCardTintOpen(0.09f, 0.16f, 0.13f, 1.0f);
const FLinearColor DTTCardTintWait(0.16f, 0.14f, 0.07f, 1.0f);
    const FLinearColor DTTCardTintWarn(0.17f, 0.11f, 0.08f, 1.0f);
    const FLinearColor DTTCyan(0.2f, 0.8f, 1.0f);
    const FLinearColor DTTGreen(0.3f, 1.0f, 0.5f);
    const FLinearColor DTTYellow(1.0f, 0.85f, 0.3f);
    const FLinearColor DTTOrange(1.0f, 0.55f, 0.1f);
    const FLinearColor DTTRed(1.0f, 0.35f, 0.3f);
    const FLinearColor DTTGrey(0.6f, 0.68f, 0.74f);
    const FLinearColor DTTDim(0.45f, 0.5f, 0.55f);
    const FLinearColor DTTSteel(0.6f, 0.7f, 0.85f);

    FString DTTStacks(int32 Stacks) { return Stacks >= 0 ? FString::Printf(TEXT("%d stacks"), Stacks) : FString(TEXT("unknown")); }
    /** A station named by its tag alone ("(Coal)") has an empty base name. */
    FString DTTStationName(const FString& Name) { return Name.IsEmpty() ? FString(TEXT("(no name)")) : Name; }
    /** The identifier number the subsystem appends to a truck's display name ("Ore -> Smelter#2147123023"): the display part changes with every target. */
    FString DTTTruckKey(const FString& Name)
    {
        int32 Hash = INDEX_NONE;
        return Name.FindLastChar(TEXT('#'), Hash) ? Name.Mid(Hash + 1) : Name;
    }
    /** Runs of the same stop (a pool lap starts with copies of its depot that only carry the target index) collapse into one chip. */
    int32 DTTRouteChips(const TArray<FString>& Stops)
    {
        int32 Chips = 0;
        for (int32 Index = 0; Index < Stops.Num(); ++Index) if (Index == 0 || Stops[Index] != Stops[Index - 1]) ++Chips;
        return Chips;
    }

    /** Card header: an icon and the title on the left, a one-line summary on the right. */
    void DTTCardHeader(FDTTCardSink& Sink, UVerticalBox* Body, const FString& Title, const FLinearColor& TitleColor, const FString& Summary, UTexture2D* Icon = nullptr)
    {
        UHorizontalBox* Row = Sink.HBox(Body, FMargin(0, 0, 0, 2));
        Sink.Icon(Row, Icon, 28, FMargin(0, 0, 8, 0));
        Sink.Text(Row, Title, 17, TitleColor, false, FMargin(0), true);
        Sink.Text(Row, Summary, 12, DTTGrey);
    }
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

    // Cards: one per resource in order of first appearance, then depots, then fuel stops.
    // The filter keeps a row whose resource or station name contains the text, any case.
    const FString Filter = DispatchFilter ? DispatchFilter->GetText().ToString().TrimStartAndEnd() : FString();
    struct FCardSpec { int32 Kind = 0; FString Resource; TArray<const FDTTDispatchRow*> Rows; };
    TArray<FCardSpec> Specs;
    TMap<FString, int32> ResourceCard;
    FCardSpec Depots; Depots.Kind = 1;
    FCardSpec FuelStops; FuelStops.Kind = 2;
    int32 Open = 0, Shown = 0;
    for (const FDTTDispatchRow& Row : Rows)
    {
        if (Row.Role != EDTTDispatchRole::Producer && !Row.bDepot && !Row.bFuelStop && Row.bOpen) ++Open;
        if (!Filter.IsEmpty() && !Row.ResourceId.Contains(Filter) && !Row.StationName.Contains(Filter)) continue;
        ++Shown;
        if (Row.bDepot) { Depots.Rows.Add(&Row); continue; }
        if (Row.bFuelStop) { FuelStops.Rows.Add(&Row); continue; }
        int32* Found = ResourceCard.Find(Row.ResourceId);
        if (!Found) { Found = &ResourceCard.Add(Row.ResourceId, Specs.Num()); Specs.AddDefaulted_GetRef().Resource = Row.ResourceId; }
        Specs[*Found].Rows.Add(&Row);
    }
    const int32 DepotCount = Depots.Rows.Num();
    if (Depots.Rows.Num() > 0) Specs.Add(MoveTemp(Depots));
    if (FuelStops.Rows.Num() > 0) Specs.Add(MoveTemp(FuelStops));
    if (Rows.Num() == 0) { FCardSpec Empty; Empty.Kind = 3; Specs.Add(Empty); }
    else if (Shown == 0) { FCardSpec Empty; Empty.Kind = 4; Specs.Add(Empty); }

    TArray<FString> Keys;
    for (const FCardSpec& Spec : Specs)
    {
        FString Key = FString::Printf(TEXT("%d|%s"), Spec.Kind, *Spec.Resource);
        for (const FDTTDispatchRow* Row : Spec.Rows) Key += FString::Printf(TEXT("|%s:%d"), *Row->DockingPathNodeGuid.ToString(EGuidFormats::Short), int32(Row->Role));
        Keys.Add(Key);
    }
    if (SummaryText)
    {
        SummaryText->SetText(FText::FromString(FString::Printf(TEXT("%d dispatch row(s), %d open, %d depot(s) - %s"),
            Rows.Num(), Open, DepotCount, IsRemoteClient() ? TEXT("server snapshot") : TEXT("live"))));
    }

    SyncCards(DispatchScrollBox, DispatchCards, Keys, [&](int32 Index, FDTTCardSink& Sink)
    {
        const FCardSpec& Spec = Specs[Index];
        switch (Spec.Kind)
        {
        case 0: PopulateResourceCard(Spec.Resource, Spec.Rows, Sink); break;
        case 1: PopulateDepotCard(Spec.Rows, Sink); break;
        case 2: PopulateFuelCard(Spec.Rows, Sink); break;
        case 4:
        {
            UVerticalBox* Body = Sink.Root(DTTCardTint);
            DTTCardHeader(Sink, Body, TEXT("Nothing matches the filter"), DTTGrey, FString());
            break;
        }
        default:
        {
            UVerticalBox* Body = Sink.Root(DTTCardTint);
            DTTCardHeader(Sink, Body, TEXT("No dispatch tags"), DTTGrey, FString());
            Sink.Text(Body, TEXT("Put (Resource) into a station name: LOAD stations produce it, UNLOAD stations request it. (Resource-N) sets the limit: "
                "a solid station is called below N stacks, a fluid one below N %. (F) marks a fuel stop, (D) a depot for the truck pool."), 13, DTTGrey, true, FMargin(0, 4, 0, 0));
            break;
        }
        }
    });
}

void UDynamicTimeTableWidget::PopulateResourceCard(const FString& Resource, const TArray<const FDTTDispatchRow*>& List, FDTTCardSink& Sink)
{
    // Yellow: open and nobody has taken it; green: taken into work; red: waiting past the limit.
    const int32 OverdueMinutes = ViewSettings().OverdueRequestMinutes;
    auto IsOverdue = [&](const FDTTDispatchRow* Row) { return OverdueMinutes > 0 && Row->bOpen && Row->ClaimOwner.IsEmpty() && Row->WaitingSeconds >= OverdueMinutes * 60; };
    int32 Producers = 0, Requesters = 0, Open = 0, Claimed = 0, Overdue = 0, Inactive = 0;
    for (const FDTTDispatchRow* Row : List)
    {
        if (!Row->bModeMatches) ++Inactive;
        if (Row->Role == EDTTDispatchRole::Producer) ++Producers;
        else { ++Requesters; if (Row->bOpen) { ++Open; if (!Row->ClaimOwner.IsEmpty()) ++Claimed; if (IsOverdue(Row)) ++Overdue; } }
    }
    FString Summary = FString::Printf(TEXT("%d producer%s, %d requester%s"), Producers, Producers == 1 ? TEXT("") : TEXT("s"), Requesters, Requesters == 1 ? TEXT("") : TEXT("s"));
    if (Open > 0) Summary += FString::Printf(TEXT("  ·  %d open, %d claimed"), Open, Claimed);
    if (Overdue > 0) Summary += FString::Printf(TEXT("  ·  %d overdue"), Overdue);
    if (Inactive > 0) Summary += FString::Printf(TEXT("  ·  %d inactive"), Inactive);
    const bool bWaiting = Open > Claimed;
    UVerticalBox* Body = Sink.Root(Overdue > 0 ? DTTCardTintWarn : (bWaiting ? DTTCardTintWait : (Open > 0 ? DTTCardTintOpen : DTTCardTint)));
    DTTCardHeader(Sink, Body, Resource, Overdue > 0 ? DTTRed : (bWaiting ? DTTYellow : (Open > 0 ? DTTGreen : DTTCyan)), Summary, List.Num() > 0 ? IconFor(List[0]->ItemClass) : nullptr);

    UHorizontalBox* Columns = Sink.HBox(Body, FMargin(0, 4, 0, 0));
    UVerticalBox* Left = Sink.Column(Columns);
    UVerticalBox* Right = Sink.Column(Columns);
    Sink.Text(Left, TEXT("Producers"), 12, DTTDim, false, FMargin(0, 0, 0, 2));
    Sink.Text(Right, TEXT("Requesters"), 12, DTTDim, false, FMargin(0, 0, 0, 2));
    Sink.Text(Left, Producers == 0 ? TEXT("none") : TEXT(""), 13, DTTDim);
    Sink.Text(Right, Requesters == 0 ? TEXT("none") : TEXT(""), 13, DTTDim);

    for (const FDTTDispatchRow* Row : List)
    {
        const TCHAR* Mode = !Row->bModeKnown ? TEXT("mode unavailable") : (Row->bLoadMode ? TEXT("LOAD") : TEXT("UNLOAD"));
        if (Row->Role == EDTTDispatchRole::Producer)
        {
            UVerticalBox* Item = Sink.VBox(Left, FMargin(0, 2, 0, 4));
            Sink.Text(Item, DTTStationName(Row->StationName), 14, Row->bModeMatches ? FLinearColor::White : DTTRed, true);
            FString Detail = FString::Printf(TEXT("stock %s  ·  %s"), *DTTStacks(Row->StockStacks), Mode);
            if (!Row->bModeMatches) Detail += TEXT("  ·  inactive: a producer must be in LOAD mode");
            Sink.Text(Item, Detail, 12, Row->bModeMatches ? DTTGrey : DTTRed, true);
            continue;
        }

        FLinearColor Status = DTTDim;
        FString StatusLine = TEXT("not open");
        if (!Row->bModeMatches) { Status = DTTRed; StatusLine = TEXT("inactive: a requester must be in UNLOAD mode"); }
        else if (Row->bOpen && Row->ClaimOwner.IsEmpty())
        {
            Status = IsOverdue(Row) ? DTTRed : DTTYellow;
            StatusLine = TEXT("open, waiting for a truck");
            if (Row->WaitingSeconds >= 60) StatusLine += FString::Printf(TEXT("  ·  %d min"), Row->WaitingSeconds / 60);
        }
        else if (Row->bOpen)
        {
            Status = DTTGreen;
            StatusLine = FString::Printf(TEXT("open, claimed by %s"), *Row->ClaimOwner);
            if (Row->PlannedStacks > 0) StatusLine += FString::Printf(TEXT("  ·  %d stacks planned"), Row->PlannedStacks);
        }
        else if (!Row->ClaimOwner.IsEmpty()) StatusLine = FString::Printf(TEXT("claimed by %s"), *Row->ClaimOwner);

        UVerticalBox* Item = Sink.VBox(Right, FMargin(0, 2, 0, 4));
        Sink.Text(Item, DTTStationName(Row->StationName), 14, Row->bModeMatches ? FLinearColor::White : DTTRed, true);
        FString Detail = Row->StockStacks >= 0
            ? (Row->CapacityStacks > 0 ? FString::Printf(TEXT("stock %d / %d stacks"), Row->StockStacks, Row->CapacityStacks) : FString::Printf(TEXT("stock %d stacks"), Row->StockStacks))
            : FString(TEXT("stock unknown"));
        if (Row->TriggerStacks >= 0)
        {
            Detail += Row->bFluid && Row->CapacityStacks > 0
                ? FString::Printf(TEXT("  ·  call below %d (%d%%)"), Row->TriggerStacks, FMath::RoundToInt(100.0f * Row->TriggerStacks / Row->CapacityStacks))
                : FString::Printf(TEXT("  ·  call below %d"), Row->TriggerStacks);
        }
        if (Row->ThresholdStacks >= 0) Detail += FString::Printf(TEXT("  ·  fill to %d"), Row->ThresholdStacks);
        Sink.Text(Item, Detail, 12, DTTGrey, true);
        const float Percent = (Row->StockStacks >= 0 && Row->CapacityStacks > 0) ? float(Row->StockStacks) / float(Row->CapacityStacks) : 0.0f;
        Sink.Bar(Item, Percent, Status);
        Sink.Text(Item, StatusLine, 12, Status, true);
    }
}

void UDynamicTimeTableWidget::PopulateDepotCard(const TArray<const FDTTDispatchRow*>& Depots, FDTTCardSink& Sink)
{
    UVerticalBox* Body = Sink.Root(DTTCardTint);
    int32 Busy = 0;
    for (const FDTTDispatchRow* Row : Depots) if (!Row->ClaimOwner.IsEmpty()) ++Busy;
    DTTCardHeader(Sink, Body, TEXT("Depots (D)"), DTTSteel, FString::Printf(TEXT("%d depot%s, %d claimed"), Depots.Num(), Depots.Num() == 1 ? TEXT("") : TEXT("s"), Busy));
    for (const FDTTDispatchRow* Row : Depots)
    {
        UVerticalBox* Item = Sink.VBox(Body, FMargin(0, 2, 0, 4));
        Sink.Text(Item, DTTStationName(Row->StationName), 14, FLinearColor::White, true);
        FString Detail = Row->ClaimOwner.IsEmpty() ? TEXT("free") : FString::Printf(TEXT("claimed by %s"), *Row->ClaimOwner);
        // The note is "docked=<truck|-> held=<yes|no>" for the chat; say it in words here.
        const int32 DockedAt = Row->Note.Find(TEXT("docked=")), HeldAt = Row->Note.Find(TEXT(" held="));
        const FString Docked = DockedAt >= 0 ? Row->Note.Mid(DockedAt + 7, HeldAt > DockedAt ? HeldAt - (DockedAt + 7) : MAX_int32) : FString();
        if (!Docked.IsEmpty() && Docked != TEXT("-")) Detail += FString::Printf(TEXT("  ·  %s docked"), *Docked);
        if (HeldAt >= 0 && Row->Note.Mid(HeldAt + 6) == TEXT("yes")) Detail += TEXT("  ·  held");
        Sink.Text(Item, Detail, 12, Row->ClaimOwner.IsEmpty() ? DTTGreen : DTTGrey, true);
    }
}

void UDynamicTimeTableWidget::PopulateFuelCard(const TArray<const FDTTDispatchRow*>& FuelStops, FDTTCardSink& Sink)
{
    UVerticalBox* Body = Sink.Root(DTTCardTint);
    DTTCardHeader(Sink, Body, TEXT("Fuel stops (F)"), DTTOrange, FString::Printf(TEXT("%d stop%s"), FuelStops.Num(), FuelStops.Num() == 1 ? TEXT("") : TEXT("s")));
    for (const FDTTDispatchRow* Row : FuelStops)
    {
        UVerticalBox* Item = Sink.VBox(Body, FMargin(0, 2, 0, 4));
        Sink.Text(Item, DTTStationName(Row->StationName), 14, FLinearColor::White, true);
    }
}

void UDynamicTimeTableWidget::PopulateTruckCard(const FDTTTruckRow& Row, FDTTCardSink& Sink)
{
    FLinearColor PhaseColor(0.25f, 0.45f, 0.35f);
    FString PhaseLabel = TEXT("Dispatch");
    if (Row.bPool)
    {
        PhaseLabel = TEXT("Pool · ") + UDynamicTimeTableSubsystem::PhaseLabel(Row.Phase);
        switch (Row.Phase)
        {
        case EDTTTruckPhase::Parked: PhaseColor = FLinearColor(0.3f, 0.32f, 0.36f); break;
        case EDTTTruckPhase::Assigned: PhaseColor = FLinearColor(0.1f, 0.4f, 0.55f); break;
        case EDTTTruckPhase::ToDepot: PhaseColor = FLinearColor(0.28f, 0.34f, 0.5f); break;
        case EDTTTruckPhase::Manual: PhaseColor = FLinearColor(0.55f, 0.3f, 0.08f); break;
        default: break;
        }
    }
    const bool bWarn = !Row.bAutopilot || (Row.bPool && Row.Phase == EDTTTruckPhase::Manual);
    UVerticalBox* Body = Sink.Root(bWarn ? DTTCardTintWarn : DTTCardTint);

    UHorizontalBox* Head = Sink.HBox(Body, FMargin(0, 0, 0, 2));
    Sink.Icon(Head, IconFor(Row.ItemClass), 28, FMargin(0, 0, 8, 0));
    Sink.Text(Head, Row.Name, 17, FLinearColor::White, false, FMargin(0, 0, 10, 0));
    Sink.Text(Head, Row.ResourceId.IsEmpty() ? TEXT("no resource") : Row.ResourceId, 14, Row.ResourceId.IsEmpty() ? DTTDim : DTTCyan, false, FMargin(0, 0, 10, 0), true);
    Sink.Badge(Head, PhaseLabel, PhaseColor);
    Sink.Badge(Head, TEXT("Autopilot off"), FLinearColor(0.6f, 0.3f, 0.05f), !Row.bAutopilot);
    Sink.Badge(Head, TEXT("Docked"), FLinearColor(0.2f, 0.35f, 0.45f), Row.bDocking);

    // Where it is going: the stops in order, the current target lit, runs of one stop as one chip.
    UHorizontalBox* RouteRow = Sink.HBox(Body, FMargin(0, 2, 0, 0));
    Sink.Text(RouteRow, TEXT("Route"), 12, DTTDim, false, FMargin(0, 0, 8, 0));
    UWrapBox* Stops = Sink.Wrap(RouteRow);
    if (Row.RouteStops.Num() == 0) Sink.Text(Stops, TEXT("no route"), 13, DTTDim);
    int32 Chips = 0;
    for (int32 Index = 0; Index < Row.RouteStops.Num(); ++Index)
    {
        if (Index > 0 && Row.RouteStops[Index] == Row.RouteStops[Index - 1]) continue;
        int32 End = Index;
        while (End + 1 < Row.RouteStops.Num() && Row.RouteStops[End + 1] == Row.RouteStops[Index]) ++End;
        if (Chips++ > 0) Sink.Text(Stops, TEXT("›"), 13, DTTDim);
        const bool bTarget = Row.TargetIndex >= Index && Row.TargetIndex <= End;
        Sink.Text(Stops, Row.RouteStops[Index], 13, bTarget ? DTTCyan : FLinearColor(0.75f, 0.8f, 0.85f));
    }

    Sink.Text(Body, Row.bPool ? FString::Printf(TEXT("depot %s"), Row.DepotName.IsEmpty() ? TEXT("-") : *Row.DepotName) : FString(), 12, DTTGrey, true, FMargin(0, 2, 0, 0));

    // Cargo: the bar shows how full the truck is, the note says what it carries and why it waits.
    const FString Cargo = Row.CargoCapacityStacks >= 0
        ? FString::Printf(TEXT("Cargo %d / %d stacks"), Row.CargoStacks, Row.CargoCapacityStacks)
        : FString(TEXT("Cargo unknown"));
    const float Percent = Row.CargoCapacityStacks > 0 ? float(FMath::Max(Row.CargoStacks, 0)) / float(Row.CargoCapacityStacks) : 0.0f;
    UHorizontalBox* CargoRow = Sink.HBox(Body, FMargin(0, 6, 0, 0));
    // What is on board when it is one item; the truck's own resource when only the count is known.
    UTexture2D* CargoIcon = IconFor(Row.CargoItemClass);
    if (!CargoIcon && Row.CargoStacks > 0) CargoIcon = IconFor(Row.ItemClass);
    Sink.Icon(CargoRow, CargoIcon, 20, FMargin(0, 0, 6, 0));
    Sink.Text(CargoRow, Cargo, 13, FLinearColor::White);
    Sink.Bar(Body, Percent, Row.CargoStacks > 0 ? DTTCyan : DTTDim);
    Sink.Text(Body, Row.CargoNote, 12, DTTGrey, true);
    Sink.Text(Body, Row.PlannedSummary.IsEmpty() ? FString() : TEXT("Planned: ") + Row.PlannedSummary, 12, FLinearColor(0.7f, 0.8f, 0.85f), true, FMargin(0, 2, 0, 0));
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

    // The key is the truck, not its display name (that changes with every target), plus what
    // decides the number of route chips; badges and optional lines are always present.
    const FString Filter = TruckFilter ? TruckFilter->GetText().ToString().TrimStartAndEnd() : FString();
    TArray<const FDTTTruckRow*> Shown;
    TArray<FString> Keys;
    int32 Pool = 0, Parked = 0;
    for (const FDTTTruckRow& Row : Rows)
    {
        if (Row.bPool) { ++Pool; if (Row.Phase == EDTTTruckPhase::Parked) ++Parked; }
        if (!Filter.IsEmpty() && !Row.Name.Contains(Filter) && !Row.ResourceId.Contains(Filter)) continue;
        Shown.Add(&Row);
        Keys.Add(FString::Printf(TEXT("%s|%d|%d"), *DTTTruckKey(Row.Name), Row.bPool ? 1 : 0, DTTRouteChips(Row.RouteStops)));
    }
    if (Shown.Num() == 0) Keys.Add(Rows.Num() == 0 ? TEXT("empty") : TEXT("nomatch"));
    if (SummaryText)
    {
        SummaryText->SetText(FText::FromString(FString::Printf(TEXT("%d truck(s), %d in the pool, %d parked - %s"),
            Rows.Num(), Pool, Parked, IsRemoteClient() ? TEXT("server snapshot") : TEXT("live"))));
    }

    SyncCards(TruckScrollBox, TruckCards, Keys, [&](int32 Index, FDTTCardSink& Sink)
    {
        if (Shown.IsValidIndex(Index)) { PopulateTruckCard(*Shown[Index], Sink); return; }
        UVerticalBox* Body = Sink.Root(DTTCardTint);
        const bool bNoTrucks = Rows.Num() == 0;
        DTTCardHeader(Sink, Body, bNoTrucks ? TEXT("No managed trucks") : TEXT("Nothing matches the filter"), DTTGrey, FString());
        Sink.Text(Body, bNoTrucks ? TEXT("A truck whose first stop is a (Resource) LOAD station runs dispatch laps; one whose route contains a (D) depot joins the pool "
            "(or /dtt park <truck>).") : TEXT(""), 13, DTTGrey, true, FMargin(0, 4, 0, 0));
    });
}
