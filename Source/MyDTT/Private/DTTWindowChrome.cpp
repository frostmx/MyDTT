#include "DTTCardSink.h"

#include "DynamicTimeTableSubsystem.h"
#include "DynamicTimeTableRemoteCallObject.h"
#include "DTTItemNames.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/CheckBox.h"
#include "Components/ComboBoxString.h"
#include "Components/ScrollBoxSlot.h"
#include "Components/SpinBox.h"
#include "Components/WidgetSwitcher.h"
#include "Blueprint/WidgetLayoutLibrary.h"
#include "Components/EditableTextBox.h"
#include "Components/ScrollBox.h"

using namespace DTTUI;

void UDynamicTimeTableWidget::ApplyWindowSize()
{
    if (!WindowSizeBox) return;
    // GetViewportSize reports pixels while the SizeBox override is in slate units: without the
    // scale division the window comes out half again as large on a 4K screen at 150 %.
    const float Scale = FMath::Max(UWidgetLayoutLibrary::GetViewportScale(this), 0.01f);
    const FVector2D Viewport = UWidgetLayoutLibrary::GetViewportSize(this) / Scale;
    // A share of what the player has, within a ceiling the columns were laid out for, and never
    // wider than the viewport itself on a screen smaller than the floor.
    const FVector2D Want(
        FMath::Min(FMath::Clamp(Viewport.X * 0.85f, 1120.0f, 1600.0f), Viewport.X),
        FMath::Min(FMath::Clamp(Viewport.Y * 0.85f, 820.0f, 1000.0f), Viewport.Y));
    if (Want.Equals(AppliedWindowSize, 1.0f)) return;
    AppliedWindowSize = Want;
    WindowSizeBox->SetWidthOverride(Want.X);
    WindowSizeBox->SetHeightOverride(Want.Y);
    // The canvas slot, not the override, is what the panel allots; both have to agree.
    if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(WindowSizeBox->Slot)) CanvasSlot->SetSize(Want);
}

void UDynamicTimeTableWidget::BuildWidgetTree()
{
    UCanvasPanel* Root = WidgetTree->ConstructWidget<UCanvasPanel>();
    WidgetTree->RootWidget = Root;

    UBorder* Dimmer = WidgetTree->ConstructWidget<UBorder>();
    Dimmer->SetBrushColor(FLinearColor(0,0,0,0.72f));
    UCanvasPanelSlot* DimmerSlot = Root->AddChildToCanvas(Dimmer);
    DimmerSlot->SetAnchors(FAnchors(0,0,1,1)); DimmerSlot->SetOffsets(FMargin(0));

    WindowSizeBox = WidgetTree->ConstructWidget<USizeBox>();
    UCanvasPanelSlot* WindowSlot = Root->AddChildToCanvas(WindowSizeBox);
    WindowSlot->SetAnchors(FAnchors(0.5f)); WindowSlot->SetAlignment(FVector2D(0.5f));
    ApplyWindowSize();

    // The window as the design draws it: a rounded panel with its own outline, a bar across the top
    // with the brand and the tabs, a status line under it, the page, and a footer.
    UBorder* WindowBorder = WidgetTree->ConstructWidget<UBorder>();
    WindowBorder->SetBrush(OutlinedBrush(DTTWindowBg, DTTWindowLine, 14.0f));
    WindowBorder->SetPadding(FMargin(1)); WindowSizeBox->SetContent(WindowBorder);
    UVerticalBox* Main = WidgetTree->ConstructWidget<UVerticalBox>(); WindowBorder->SetContent(Main);

    UBorder* TopBar = WidgetTree->ConstructWidget<UBorder>();
    TopBar->SetBrush(CornerBrush(DTTTopBar, FVector4(13, 13, 0, 0)));
    TopBar->SetPadding(FMargin(16, 0));
    USizeBox* TopBox = WidgetTree->ConstructWidget<USizeBox>(); TopBox->SetHeightOverride(56); TopBox->SetContent(TopBar);
    Main->AddChildToVerticalBox(TopBox);
    UHorizontalBox* Top = WidgetTree->ConstructWidget<UHorizontalBox>(); TopBar->SetContent(Top);
    if (UBorderSlot* S = Cast<UBorderSlot>(Top->Slot)) S->SetVerticalAlignment(VAlign_Center);
    AddRowItem(Top, MakeText(WidgetTree, TEXT("MyDTT"), 18, DTTCyan), 0)->SetPadding(FMargin(0, 0, 18, 0));
    UHorizontalBox* Tabs = WidgetTree->ConstructWidget<UHorizontalBox>();
    AddRowItem(Top, Tabs, 0, true);
    // The order of the pages, and the order of EDTTTab: the two must agree.
    DispatchTabButton = MakeButton(WidgetTree, TEXT("Dispatch"), EDTTButton::Tab); AddRowItem(Tabs, DispatchTabButton, 1);
    TrucksTabButton = MakeButton(WidgetTree, TEXT("Trucks"), EDTTButton::Tab); AddRowItem(Tabs, TrucksTabButton, 1);
    StationsTabButton = MakeButton(WidgetTree, TEXT("Stations"), EDTTButton::Tab); AddRowItem(Tabs, StationsTabButton, 1);
    TimetableTabButton = MakeButton(WidgetTree, TEXT("Timetable"), EDTTButton::Tab); AddRowItem(Tabs, TimetableTabButton, 1);
    GroupsTabButton = MakeButton(WidgetTree, TEXT("Groups"), EDTTButton::Tab); AddRowItem(Tabs, GroupsTabButton, 1);
    SettingsTabButton = MakeButton(WidgetTree, TEXT("Settings"), EDTTButton::Tab); AddRowItem(Tabs, SettingsTabButton, 1);
    CloseButton = MakeButton(WidgetTree, TEXT("Close")); AddRowItem(Top, CloseButton, 0);

    // The status line: counts in their meaning's colour, words in grey. A fixed set of text blocks
    // rewritten on every refresh, never rebuilt.
    UBorder* SubBar = WidgetTree->ConstructWidget<UBorder>();
    SubBar->SetBrush(FlatBrush(DTTSubBar, 0.0f));
    SubBar->SetPadding(FMargin(18, 0));
    USizeBox* SubBox = WidgetTree->ConstructWidget<USizeBox>(); SubBox->SetHeightOverride(34); SubBox->SetContent(SubBar);
    Main->AddChildToVerticalBox(SubBox);
    UHorizontalBox* Sub = WidgetTree->ConstructWidget<UHorizontalBox>(); SubBar->SetContent(Sub);
    if (UBorderSlot* S = Cast<UBorderSlot>(Sub->Slot)) S->SetVerticalAlignment(VAlign_Center);
    StatusParts.Reset();
    for (int32 Index = 0; Index < MaxStatusParts; ++Index)
    {
        UTextBlock* Part = MakeText(WidgetTree, FString(), 13, DTTGrey);
        Part->SetVisibility(ESlateVisibility::Collapsed);
        AddRowItem(Sub, Part, 0);
        StatusParts.Add(Part);
    }
    UTextBlock* Spacer = MakeText(WidgetTree, FString(), 13, DTTGrey);
    AddRowItem(Sub, Spacer, 0, true);
    SummaryText = MakeText(WidgetTree, TEXT("Loading..."), 12, DTTDim);
    AddRowItem(Sub, SummaryText, 0);

    Pages = WidgetTree->ConstructWidget<UWidgetSwitcher>();
    UVerticalBoxSlot* PagesSlot = Main->AddChildToVerticalBox(Pages);
    PagesSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
    PagesSlot->SetPadding(FMargin(16, 12, 16, 8));

    UHorizontalBox* DispatchHeader = nullptr;
    Pages->AddChild(BuildCardPage(DispatchScrollBox, DispatchFilter, DispatchHeader));
    BuildDispatchHeader(DispatchHeader);
    DispatchFilter->OnTextChanged.AddDynamic(this, &UDynamicTimeTableWidget::HandleDispatchFilterChanged);
    UHorizontalBox* TruckHeader = nullptr;
    Pages->AddChild(BuildCardPage(TruckScrollBox, TruckFilter, TruckHeader));
    BuildTruckHeader(TruckHeader);
    TruckFilter->OnTextChanged.AddDynamic(this, &UDynamicTimeTableWidget::HandleTruckFilterChanged);
    UHorizontalBox* StationHeader = nullptr;
    UHorizontalBox* StationTools = nullptr;
    UHorizontalBox* StationFilters = nullptr;
    Pages->AddChild(BuildCardPage(StationScrollBox, StationFilter, StationHeader, &StationTools, &StationFilters));
    BuildStationFilters(StationFilters);
    BuildStationTools(StationTools);
    BuildStationHeader(StationHeader);
    StationFilter->OnTextChanged.AddDynamic(this, &UDynamicTimeTableWidget::HandleStationFilterChanged);
    Pages->AddChild(BuildTimetablePage());
    UVerticalBox* GroupsPage = WidgetTree->ConstructWidget<UVerticalBox>();
    Pages->AddChild(GroupsPage);
    BuildEditorPanel(GroupsPage);
    GroupScrollBox = WidgetTree->ConstructWidget<UScrollBox>();
    UVerticalBoxSlot* ScrollSlot = GroupsPage->AddChildToVerticalBox(GroupScrollBox);
    ScrollSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill)); ScrollSlot->SetPadding(FMargin(5,8));
    UVerticalBox* SettingsPage = WidgetTree->ConstructWidget<UVerticalBox>();
    Pages->AddChild(SettingsPage);
    BuildSettingsPanel(SettingsPage);
    Pages->SetActiveWidgetIndex(static_cast<int32>(EDTTTab::Dispatch));

    UBorder* FootBar = WidgetTree->ConstructWidget<UBorder>();
    FootBar->SetBrush(CornerBrush(DTTSubBar, FVector4(0, 0, 13, 13)));
    FootBar->SetPadding(FMargin(18, 0));
    USizeBox* FootBox = WidgetTree->ConstructWidget<USizeBox>(); FootBox->SetHeightOverride(38); FootBox->SetContent(FootBar);
    Main->AddChildToVerticalBox(FootBox);
    UHorizontalBox* Footer = WidgetTree->ConstructWidget<UHorizontalBox>(); FootBar->SetContent(Footer);
    if (UBorderSlot* S = Cast<UBorderSlot>(Footer->Slot)) S->SetVerticalAlignment(VAlign_Center);
    // The status of the last thing asked for, where it can be read from any tab: the group
    // editor's own line is on the Groups page, and a row button on another page said nothing.
    StatusText = MakeText(WidgetTree, TEXT("Ready."), 13, DTTGrey);
    AddRowItem(Footer, StatusText, 0, true);
    RecalculateButton = MakeButton(WidgetTree, TEXT("Rebuild bookings")); AddRowItem(Footer, RecalculateButton, 0);
}

UVerticalBox* UDynamicTimeTableWidget::BuildCardPage(TObjectPtr<UScrollBox>& OutScroll, TObjectPtr<UEditableTextBox>& OutFilter,
    UHorizontalBox*& OutHeader, UHorizontalBox** OutTools, UHorizontalBox** OutFilterRow)
{
    UVerticalBox* Page = WidgetTree->ConstructWidget<UVerticalBox>();
    UHorizontalBox* FilterRow = WidgetTree->ConstructWidget<UHorizontalBox>();
    Page->AddChildToVerticalBox(FilterRow)->SetPadding(FMargin(0, 0, 0, 8));
    // A search box of its own width, as the design has it: a filter across the whole window
    // leaves no room for what a page filters by besides the words.
    OutFilter = WidgetTree->ConstructWidget<UEditableTextBox>();
    OutFilter->SetHintText(FText::FromString(TEXT("filter: name or resource")));
    StyleInput(OutFilter, 13);
    USizeBox* FilterBox = WidgetTree->ConstructWidget<USizeBox>();
    FilterBox->SetWidthOverride(280.0f);
    FilterBox->SetContent(OutFilter);
    AddRowItem(FilterRow, FilterBox, 0)->SetPadding(FMargin(0, 0, 10, 0));
    if (OutFilterRow) *OutFilterRow = FilterRow;
    // A page that acts on more than one row at a time puts those controls here, between the
    // filter and the captions, where they are read before the table rather than hunted for in it.
    if (OutTools)
    {
        *OutTools = WidgetTree->ConstructWidget<UHorizontalBox>();
        Page->AddChildToVerticalBox(*OutTools)->SetPadding(FMargin(8, 0, 8, 4));
    }
    // The captions stay above the scroll box rather than scrolling away with the first screen
    // of forty resources.
    OutHeader = WidgetTree->ConstructWidget<UHorizontalBox>();
    // 2 from the card's own slot plus 6 of the row button's padding: the captions have to
    // start exactly where the first column of a row starts.
    Page->AddChildToVerticalBox(OutHeader)->SetPadding(FMargin(8, 2, 8, 2));
    OutScroll = WidgetTree->ConstructWidget<UScrollBox>();
    Page->AddChildToVerticalBox(OutScroll)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
    return Page;
}

void UDynamicTimeTableWidget::HandleDispatchFilterChanged(const FText&) { RefreshDispatchDisplay(); }
void UDynamicTimeTableWidget::HandleTruckFilterChanged(const FText&) { RefreshTruckDisplay(); }
void UDynamicTimeTableWidget::HandleStationFilterChanged(const FText&) { RefreshStationDisplay(); }
void UDynamicTimeTableWidget::HandleTimetableFilterChanged(const FText&) { RefreshTimetableDisplay(); }

void UDynamicTimeTableWidget::BuildEditorPanel(UVerticalBox* Main)
{
    UBorder* EditorBorder = WidgetTree->ConstructWidget<UBorder>();
    EditorBorder->SetBrush(OutlinedBrush(DTTPane, Hex(0x3a4f5b), 10.0f)); EditorBorder->SetPadding(FMargin(14, 10));
    Main->AddChildToVerticalBox(EditorBorder)->SetPadding(FMargin(0,0,0,10));
    UVerticalBox* Editor = WidgetTree->ConstructWidget<UVerticalBox>(); EditorBorder->SetContent(Editor);
    Editor->AddChildToVerticalBox(MakeText(WidgetTree,TEXT("Group configuration"),16,DTTCyan))->SetPadding(FMargin(0,0,0,4));

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
    TargetSlotsInput=WidgetTree->ConstructWidget<USpinBox>(); TargetSlotsInput->SetMinValue(1); TargetSlotsInput->SetMaxValue(999); TargetSlotsInput->SetMinSliderValue(1); TargetSlotsInput->SetMaxSliderValue(999); TargetSlotsInput->SetDelta(1); TargetSlotsInput->SetAlwaysUsesDeltaSnap(true); TargetSlotsInput->SetMinFractionalDigits(0); TargetSlotsInput->SetMaxFractionalDigits(0); TargetSlotsInput->SetValue(1);
    AddRowItem(StationRow,TargetSlotsInput); ApplySlotsButton=MakeButton(WidgetTree,TEXT("Apply target slots")); AddRowItem(StationRow,ApplySlotsButton);
    RemoveStationButton=MakeButton(WidgetTree,TEXT("Remove station")); AddRowItem(StationRow,RemoveStationButton);

    EditorStatusText=MakeText(WidgetTree,TEXT("Ready."),13,DTTGrey); Editor->AddChildToVerticalBox(EditorStatusText)->SetPadding(FMargin(5));
    StyleInput(NewGroupNameInput); StyleInput(GroupNameInput); StyleSpin(TargetSlotsInput);
    for (UComboBoxString* Combo : { NewGroupKindSelector.Get(), GroupSelector.Get(), AvailableStationSelector.Get(), GroupStationSelector.Get() }) StyleCombo(Combo);
}

void UDynamicTimeTableWidget::BuildSettingsPanel(UVerticalBox* Page)
{
    // Header and the Save/Defaults row stay put; the rows scroll between them. The border fills
    // the page and clips, so a long list can never paint over the window's footer again.
    UBorder* Border = WidgetTree->ConstructWidget<UBorder>();
    Border->SetBrush(OutlinedBrush(DTTPane, Hex(0x3a4f5b), 10.0f)); Border->SetPadding(FMargin(14, 10));
    Border->SetClipping(EWidgetClipping::ClipToBounds);
    UVerticalBoxSlot* BorderSlot = Page->AddChildToVerticalBox(Border);
    BorderSlot->SetPadding(FMargin(0)); BorderSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
    UVerticalBox* Box = WidgetTree->ConstructWidget<UVerticalBox>(); Border->SetContent(Box);
    Box->AddChildToVerticalBox(MakeText(WidgetTree,TEXT("Settings"),16,DTTCyan));
    Box->AddChildToVerticalBox(MakeText(WidgetTree,TEXT("Saved with the world; the server's values apply. Save writes them, Defaults restores the shipped values."),12,DTTGrey))->SetPadding(FMargin(0,0,0,4));
    UScrollBox* Rows = WidgetTree->ConstructWidget<UScrollBox>();
    UVerticalBoxSlot* RowsSlot = Box->AddChildToVerticalBox(Rows);
    RowsSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill)); RowsSlot->SetPadding(FMargin(0,2));

    auto Section = [&](const TCHAR* Title)
    {
        UTextBlock* T = MakeText(WidgetTree,FString(Title).ToUpper(),12,DTTCyan);
        Rows->AddChild(T);
        if (UScrollBoxSlot* S = Cast<UScrollBoxSlot>(T->Slot)) S->SetPadding(FMargin(0,8,0,2));
    };
    auto Row = [&](const TCHAR* Label, const TCHAR* Hint, UWidget* Control)
    {
        UHorizontalBox* R = WidgetTree->ConstructWidget<UHorizontalBox>();
        Rows->AddChild(R);
        if (UScrollBoxSlot* S = Cast<UScrollBoxSlot>(R->Slot)) S->SetPadding(FMargin(0,1,8,1));
        UVerticalBox* Text = WidgetTree->ConstructWidget<UVerticalBox>();
        Text->AddChildToVerticalBox(MakeText(WidgetTree,Label,14,DTTText));
        UTextBlock* H = MakeText(WidgetTree,Hint,11,DTTDim); H->SetAutoWrapText(true);
        Text->AddChildToVerticalBox(H);
        AddRowItem(R,Text,3,true);
        AddRowItem(R,Control,3);
    };
    auto Spin = [&](const TCHAR* Label, const TCHAR* Hint, float Min, float Max, float Delta) -> USpinBox*
    {
        USpinBox* S = WidgetTree->ConstructWidget<USpinBox>();
        S->SetMinValue(Min); S->SetMaxValue(Max); S->SetMinSliderValue(Min); S->SetMaxSliderValue(Max);
        S->SetDelta(Delta); S->SetAlwaysUsesDeltaSnap(true); S->SetMinFractionalDigits(Delta < 1.0f ? 1 : 0); S->SetMaxFractionalDigits(Delta < 1.0f ? 2 : 0);
        StyleSpin(S);
        USizeBox* Size = WidgetTree->ConstructWidget<USizeBox>(); Size->SetWidthOverride(120); Size->SetContent(S);
        Row(Label, Hint, Size);
        return S;
    };
    auto Check = [&](const TCHAR* Label, const TCHAR* Hint) -> UCheckBox*
    {
        UCheckBox* C = WidgetTree->ConstructWidget<UCheckBox>();
        StyleCheck(C);
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

    Section(TEXT("Naming"));
    SettingAutoName = Check(TEXT("Name stations from their belts"), TEXT("Off by default. On: the mod reads the belts around every dock, writes \"(Resource) -240/m -> Product\" into the name and sets the LOAD/UNLOAD switch to match. Text you wrote yourself is kept; a name ending in a full stop is left alone entirely."));
    SettingAutoNameDebounce = Spin(TEXT("Rewrite after, seconds"), TEXT("Quiet time after the last build or rename before a station is walked again."), 0.5f, 30, 0.5f);
    SettingAutoNameSweep = Spin(TEXT("Full sweep every, seconds"), TEXT("Every station is checked this often. This is what notices an overclock change, which no hook reports."), 10, 600, 10);
    SettingAutoNameMaxNodes = Spin(TEXT("Walk at most, buildings"), TEXT("A chain longer than this leaves the station unnamed instead of costing a frame."), 50, 5000, 50);
    SettingAutoNameMaxProducts = Spin(TEXT("Products in the name"), TEXT("How many final products the name lists before it ends in an ellipsis."), 1, 8, 1);
    // Built from this client's own language list: exactly what its options menu offers.
    SettingNamingLanguage = WidgetTree->ConstructWidget<UComboBoxString>();
    StyleCombo(SettingNamingLanguage);
    NamingLanguageCodes.Reset();
    for (const FDTTLanguageOption& Language : FDTTItemNames::ListLanguages())
    {
        SettingNamingLanguage->AddOption(Language.Label);
        NamingLanguageCodes.Add(Language.Code);
    }
    {
        USizeBox* Size = WidgetTree->ConstructWidget<USizeBox>(); Size->SetWidthOverride(220); Size->SetContent(SettingNamingLanguage);
        Row(TEXT("Language of the names"), TEXT("Which of the game's languages item names are written in; English by default. A word of your own for a resource is kept. A game name written in another language is rewritten in this one when you change this setting or run /dtt autoname apply."), Size);
    }

    UHorizontalBox* Buttons = WidgetTree->ConstructWidget<UHorizontalBox>(); Box->AddChildToVerticalBox(Buttons)->SetPadding(FMargin(0,6,0,0));
    SettingsSaveButton = MakeButton(WidgetTree,TEXT("Save")); AddRowItem(Buttons,SettingsSaveButton);
    SettingsDefaultsButton = MakeButton(WidgetTree,TEXT("Defaults")); AddRowItem(Buttons,SettingsDefaultsButton);
    SettingsStatusText = MakeText(WidgetTree,TEXT("Ready."),13,DTTGrey); Box->AddChildToVerticalBox(SettingsStatusText)->SetPadding(FMargin(5));
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
    SettingAutoName->SetIsChecked(V.bAutoNameStations);
    SettingAutoNameDebounce->SetValue(V.AutoNameDebounceSeconds);
    SettingAutoNameSweep->SetValue(V.AutoNameSweepSeconds);
    SettingAutoNameMaxNodes->SetValue(float(V.AutoNameMaxNodes));
    SettingAutoNameMaxProducts->SetValue(float(V.AutoNameMaxProducts));
    if (SettingNamingLanguage)
    {
        int32 Index = NamingLanguageCodes.IndexOfByKey(V.NamingLanguage);
        if (Index == INDEX_NONE)
        {
            // A language this client does not know is shown by its code, so Save keeps it as is.
            SettingNamingLanguage->AddOption(V.NamingLanguage);
            NamingLanguageCodes.Add(V.NamingLanguage);
            Index = NamingLanguageCodes.Num() - 1;
        }
        SettingNamingLanguage->SetSelectedIndex(Index);
    }
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
    V.bAutoNameStations = SettingAutoName->IsChecked();
    V.AutoNameDebounceSeconds = SettingAutoNameDebounce->GetValue();
    V.AutoNameSweepSeconds = SettingAutoNameSweep->GetValue();
    V.AutoNameMaxNodes = FMath::RoundToInt(SettingAutoNameMaxNodes->GetValue());
    V.AutoNameMaxProducts = FMath::RoundToInt(SettingAutoNameMaxProducts->GetValue());
    if (SettingNamingLanguage && NamingLanguageCodes.IsValidIndex(SettingNamingLanguage->GetSelectedIndex()))
    {
        V.NamingLanguage = NamingLanguageCodes[SettingNamingLanguage->GetSelectedIndex()];
    }
    V.Sanitize();
    return V;
}

void UDynamicTimeTableWidget::HandleSettingSpinChanged(float) { if (!bRefreshingControls) bSettingsEdited = true; }
void UDynamicTimeTableWidget::HandleSettingCheckChanged(bool) { if (!bRefreshingControls) bSettingsEdited = true; }
void UDynamicTimeTableWidget::HandleSettingComboChanged(FString, ESelectInfo::Type) { if (!bRefreshingControls) bSettingsEdited = true; }
void UDynamicTimeTableWidget::HandleSettingsTabClicked() { SetActiveTab(EDTTTab::Settings); RefreshSettingsControls(true); }
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
    if (SettingNamingLanguage) SettingNamingLanguage->SetSelectedIndex(FMath::Max(0, NamingLanguageCodes.IndexOfByKey(Defaults.NamingLanguage)));
    bRefreshingControls = false;
    bSettingsEdited = true;
    SetEditorStatus(TEXT("Defaults loaded into the controls; Save to apply."), false);
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
    // Only the Groups page speaks on the status line: this runs on every refresh, whatever tab is open.
    if (ActiveTab == EDTTTab::Groups)
    {
        int32 Stations = 0;
        for (const FDTTStationGroup& G : Groups) Stations += G.Stations.Num();
        SetStatusLine(FDTTStatusLine().Count(Groups.Num(), TEXT("groups")).Count(Stations, TEXT("stations")), FString());
    }

    for (const FDTTStationGroup& Group : Groups)
    {
        RuntimeGroupIds.Add(Group.GroupId);
        RuntimeStationCounts.Add(Group.Stations.Num());

        UBorder* Border = WidgetTree->ConstructWidget<UBorder>();
        Border->SetBrush(FlatBrush(FLinearColor::White, 8.0f));
        Border->SetBrushColor(DTTCardTint);
        Border->SetPadding(FMargin(14, 10));
        GroupScrollBox->AddChild(Border);
        if (UScrollBoxSlot* S = Cast<UScrollBoxSlot>(Border->Slot)) S->SetPadding(FMargin(0, 0, 0, 8));
        RuntimeGroupBorders.Add(Border);

        UVerticalBox* Panel = WidgetTree->ConstructWidget<UVerticalBox>();
        Border->SetContent(Panel);

        UTextBlock* GroupText = MakeText(WidgetTree, Group.GroupName, 16, DTTCyan);
        Panel->AddChildToVerticalBox(GroupText);
        RuntimeGroupTexts.Add(GroupText);

        for (const FDTTStationEntry& Entry : Group.Stations)
        {
            UTextBlock* StationText = MakeText(WidgetTree,
                UDynamicTimeTableSubsystem::StripStationTag(Entry.DisplayName), 13, DTTText);
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
    // Only the Groups page speaks on the status line: this runs on every refresh, whatever tab is open.
    if (ActiveTab == EDTTTab::Groups)
    {
        int32 Stations = 0;
        for (const FDTTStationGroup& G : Groups) Stations += G.Stations.Num();
        SetStatusLine(FDTTStatusLine().Count(Groups.Num(), TEXT("groups")).Count(Stations, TEXT("stations")), FString());
    }

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
            bOverloaded ? DTTCardTintWarn : DTTCardTint);
        RuntimeGroupTexts[GroupIndex]->SetText(FText::FromString(FString::Printf(
            TEXT("%s  [%s]    %d/%d%s"),
            *Group.GroupName,
            Group.Kind == EDTTGroupKind::Truck ? TEXT("Trucks") : TEXT("Trains"),
            Assigned,
            Target,
            bOverloaded ? TEXT("  OVERLOAD") : TEXT(""))));
        RuntimeGroupTexts[GroupIndex]->SetColorAndOpacity(FSlateColor(
            bOverloaded ? DTTRed : DTTCyan));

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
                bStationOverloaded ? DTTRed
                    : bStationUnreachable ? DTTOrange
                    : DTTText));
        }
    }
}
void UDynamicTimeTableWidget::RefreshEditorControls(){if(bRefreshingControls)return;bRefreshingControls=true;const auto&Groups=ViewGroups();if(!SelectedGroupId.IsValid()||!Groups.ContainsByPredicate([&](const FDTTStationGroup&G){return G.GroupId==SelectedGroupId;}))SelectedGroupId=Groups.Num()?Groups[0].GroupId:FGuid();GroupSelector->ClearOptions();GroupOptionIds.Reset();const FDTTStationGroup*Selected=nullptr;for(const auto&G:Groups){GroupSelector->AddOption(G.GroupName);GroupOptionIds.Add(G.GroupId);if(G.GroupId==SelectedGroupId)Selected=&G;}if(Selected){GroupSelector->SetSelectedOption(Selected->GroupName);GroupNameInput->SetText(FText::FromString(Selected->GroupName));}else GroupNameInput->SetText(FText::GetEmpty());GroupStationSelector->ClearOptions();if(Selected)for(const auto&E:Selected->Stations)GroupStationSelector->AddOption(E.DisplayName);if(GroupStationSelector->GetOptionCount()>0)GroupStationSelector->SetSelectedIndex(0);const EDTTGroupKind Kind=Selected?Selected->Kind:EDTTGroupKind::Train;const bool bTruck=Kind==EDTTGroupKind::Truck;TArray<FString>All;if(IsRemoteClient()&&RemoteCallObject)All=bTruck?RemoteCallObject->GetSnapshot().AvailableTruckStations:RemoteCallObject->GetSnapshot().AvailableStations;else if(auto*S=GetWorld()->GetSubsystem<UDynamicTimeTableSubsystem>()){if(bTruck)S->GetAllDockingStationNames(All);else S->GetAllVanillaStationNames(All);}AvailableStationSelector->ClearOptions();// A rail station and a docking station may share a name, so only groups of the same kind claim it.
for(const FString&N:All)if(!Groups.ContainsByPredicate([&](const FDTTStationGroup&G){return G.Kind==Kind&&G.Stations.ContainsByPredicate([&](const FDTTStationEntry&E){return E.DisplayName==N;});}))AvailableStationSelector->AddOption(N);if(AvailableStationSelector->GetOptionCount()>0)AvailableStationSelector->SetSelectedIndex(0);bRefreshingControls=false;HandleGroupStationSelectionChanged(GroupStationSelector->GetSelectedOption(),ESelectInfo::Direct);}
void UDynamicTimeTableWidget::SetEditorStatus(const FString&M,bool E){const FSlateColor C(E?DTTRed:DTTGreen);EditorStatusText->SetText(FText::FromString(M));EditorStatusText->SetColorAndOpacity(C);if(SettingsStatusText){SettingsStatusText->SetText(FText::FromString(M));SettingsStatusText->SetColorAndOpacity(C);}if(StatusText){StatusText->SetText(FText::FromString(M));StatusText->SetColorAndOpacity(C);}}FString UDynamicTimeTableWidget::GetSelectedGroupStationName()const{return GroupStationSelector?GroupStationSelector->GetSelectedOption():FString();}
void UDynamicTimeTableWidget::HandleGroupSelectionChanged(FString Item,ESelectInfo::Type){if(bRefreshingControls)return;int32 I=GroupSelector->FindOptionIndex(Item);if(GroupOptionIds.IsValidIndex(I)){SelectedGroupId=GroupOptionIds[I];RefreshEditorControls();}}
void UDynamicTimeTableWidget::HandleGroupStationSelectionChanged(FString Item,ESelectInfo::Type){if(Item.IsEmpty())return;if(const auto*G=ViewGroups().FindByPredicate([&](const FDTTStationGroup&X){return X.GroupId==SelectedGroupId;}))if(const auto*E=G->Stations.FindByPredicate([&](const FDTTStationEntry&X){return X.DisplayName==Item;}))TargetSlotsInput->SetValue(E->TargetSlots);}
void UDynamicTimeTableWidget::HandleCreateGroupClicked(){FString N=NewGroupNameInput->GetText().ToString().TrimStartAndEnd();if(N.IsEmpty()){SetEditorStatus(TEXT("Enter a group name."),true);return;}const EDTTGroupKind Kind=(NewGroupKindSelector&&NewGroupKindSelector->GetSelectedIndex()==1)?EDTTGroupKind::Truck:EDTTGroupKind::Train;if(IsRemoteClient()){if(RemoteCallObject)RemoteCallObject->RequestCreateGroup(N,Kind);else SetEditorStatus(TEXT("Server connection unavailable."),true);}else if(auto*S=GetWorld()->GetSubsystem<UDynamicTimeTableSubsystem>()){SelectedGroupId=S->CreateGroup(N,Kind);RefreshData();}NewGroupNameInput->SetText(FText::GetEmpty());}
void UDynamicTimeTableWidget::HandleRenameGroupClicked(){FString N=GroupNameInput->GetText().ToString().TrimStartAndEnd();if(IsRemoteClient()){if(RemoteCallObject)RemoteCallObject->RequestRenameGroup(SelectedGroupId,N);}else if(auto*S=GetWorld()->GetSubsystem<UDynamicTimeTableSubsystem>())SetEditorStatus(S->RenameGroup(SelectedGroupId,N)?TEXT("Group renamed."):TEXT("Rename failed."));}
void UDynamicTimeTableWidget::HandleDeleteGroupClicked(){if(IsRemoteClient()){if(RemoteCallObject)RemoteCallObject->RequestDeleteGroup(SelectedGroupId);}else if(auto*S=GetWorld()->GetSubsystem<UDynamicTimeTableSubsystem>()){S->RemoveGroup(SelectedGroupId);SelectedGroupId=FGuid();RefreshData();}}
void UDynamicTimeTableWidget::HandleAddStationClicked(){FString N=AvailableStationSelector->GetSelectedOption();if(IsRemoteClient()){if(RemoteCallObject)RemoteCallObject->RequestAddStation(SelectedGroupId,N);}else if(auto*S=GetWorld()->GetSubsystem<UDynamicTimeTableSubsystem>())S->AddStationByName(SelectedGroupId,N,1);}
void UDynamicTimeTableWidget::HandleApplySlotsClicked(){int32 Slots=FMath::Clamp(FMath::RoundToInt(TargetSlotsInput->GetValue()),1,999);if(IsRemoteClient()){if(RemoteCallObject)RemoteCallObject->RequestSetTargetSlots(SelectedGroupId,GetSelectedGroupStationName(),Slots);}else if(auto*S=GetWorld()->GetSubsystem<UDynamicTimeTableSubsystem>())S->SetTargetSlots(SelectedGroupId,GetSelectedGroupStationName(),Slots);}
void UDynamicTimeTableWidget::HandleRemoveStationClicked(){if(IsRemoteClient()){if(RemoteCallObject)RemoteCallObject->RequestRemoveStation(SelectedGroupId,GetSelectedGroupStationName());}else if(auto*S=GetWorld()->GetSubsystem<UDynamicTimeTableSubsystem>())S->RemoveStationByName(SelectedGroupId,GetSelectedGroupStationName());}
void UDynamicTimeTableWidget::HandleRecalculateClicked(){if(IsRemoteClient()){if(RemoteCallObject)RemoteCallObject->RequestRecalculate();}else if(auto*S=GetWorld()->GetSubsystem<UDynamicTimeTableSubsystem>())S->RecalculateBookingsFromVanilla();}
