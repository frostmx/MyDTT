#include "DTTCardSink.h"

#include "DynamicTimeTableSubsystem.h"
#include "DynamicTimeTableRemoteCallObject.h"
#include "WheeledVehicles/FGDockingStationIdentifier.h"
#include "Resources/FGItemDescriptor.h"
#include "Components/EditableTextBox.h"
#include "Components/ScrollBox.h"
#include "Components/SpinBox.h"
#include "Components/ComboBoxString.h"
#include "Algo/StableSort.h"

using namespace DTTUI;

namespace
{
    FString StationWaitText(int32 Seconds)
    {
        return Seconds > 0 ? FString::Printf(TEXT("%d:%02d"), Seconds / 60, Seconds % 60) : FString(TEXT("-"));
    }

    float StationLevel(int32 Level, int32 Capacity)
    {
        return (Level >= 0 && Capacity > 0) ? FMath::Clamp(float(Level) / float(Capacity), 0.0f, 1.0f) : -1.0f;
    }

    /** What the row is, in one word: this is the column a player scans the table by. */
    FString RoleText(const FDTTStationRow& Row)
    {
        if (Row.Flags & EDTTStationFlags::Depot) return TEXT("depot");
        if (Row.Flags & EDTTStationFlags::FuelStop) return TEXT("fuel");
        switch (Row.Role)
        {
        case EDTTDispatchRole::Producer: return TEXT("producer");
        case EDTTDispatchRole::Requester: return TEXT("requester");
        default: return FString();
        }
    }

    EDTTPill RolePill(const FDTTStationRow& Row)
    {
        if (Row.Flags & EDTTStationFlags::Depot) return EDTTPill::Depot;
        if (Row.Flags & EDTTStationFlags::FuelStop) return EDTTPill::Fuel;
        return Row.Role == EDTTDispatchRole::Requester ? EDTTPill::Requester : EDTTPill::Producer;
    }

    /** A tag the dock's mode works against: the one thing on this page that is plainly broken. */
    bool NeedsFix(const FDTTStationRow& Row)
    {
        return (Row.Flags & EDTTStationFlags::ModeKnown) && !(Row.Flags & EDTTStationFlags::ModeMatches)
            && !(Row.Flags & (EDTTStationFlags::Depot | EDTTStationFlags::FuelStop));
    }

    bool MatchesRole(const FDTTStationRow& Row, EDTTStationRoleFilter Filter)
    {
        const bool bDepot = (Row.Flags & EDTTStationFlags::Depot) != 0;
        const bool bFuel = (Row.Flags & EDTTStationFlags::FuelStop) != 0;
        switch (Filter)
        {
        case EDTTStationRoleFilter::Producers: return !bDepot && !bFuel && Row.Role == EDTTDispatchRole::Producer;
        case EDTTStationRoleFilter::Requesters: return !bDepot && !bFuel && Row.Role == EDTTDispatchRole::Requester;
        case EDTTStationRoleFilter::Depots: return bDepot;
        case EDTTStationRoleFilter::Fuel: return bFuel;
        case EDTTStationRoleFilter::NeedsFix: return NeedsFix(Row);
        default: return true;
        }
    }

    /** The State column as an order: what needs a hand first, what is quiet last. */
    int32 StateRank(const FDTTStationRow& Row, int32 OverdueMinutes)
    {
        const bool bOpen = (Row.Flags & EDTTStationFlags::Open) != 0;
        if (NeedsFix(Row)) return 0;
        if (bOpen && Row.ClaimOwner.IsEmpty() && OverdueMinutes > 0 && Row.WaitingSeconds >= OverdueMinutes * 60) return 1;
        if (bOpen && Row.ClaimOwner.IsEmpty()) return 2;
        if (bOpen) return 3;
        if (!Row.ClaimOwner.IsEmpty()) return 4;
        return 5;
    }

    int32 ModeRank(const FDTTStationRow& Row)
    {
        if (!(Row.Flags & EDTTStationFlags::ModeKnown)) return 0;
        return (Row.Flags & EDTTStationFlags::LoadMode) ? 1 : 2;
    }

    template <typename T>
    int32 Order(const T& A, const T& B) { return A < B ? -1 : (B < A ? 1 : 0); }

    const TCHAR* AllResourcesOption = TEXT("All resources");
    const TCHAR* RoleChipLabels[] = { TEXT("All"), TEXT("Producers"), TEXT("Requesters"), TEXT("Depots"), TEXT("Fuel"), TEXT("Needs a fix") };
    static_assert(UE_ARRAY_COUNT(RoleChipLabels) == int32(EDTTStationRoleFilter::Count), "a label per chip");
}

void UDynamicTimeTableWidget::BuildStationHeader(UHorizontalBox* Row)
{
    if (!Row) return;
    // One caption per cell of the row, in the same order and with the same widths and margins.
    // A caption that stands over the wrong column is worse than no caption, and the fill column
    // takes up every point the two sides disagree about.
    // The captions sort: a click orders the table by that column, the next click reverses it.
    StationSortLabels.SetNum(int32(EDTTStationSort::Count));
    StationSortCaptions.SetNum(int32(EDTTStationSort::Count));
    auto Sortable = [this, Row](EDTTStationSort Column, const TCHAR* Caption, float Width,
        EHorizontalAlignment Align = HAlign_Left, const FMargin& Margin = FMargin(0, 0, 6, 0))
    {
        FDTTRowTarget Target;
        Target.Action = EDTTRowAction::StationSort;
        Target.Value = int32(Column);
        StationSortLabels[int32(Column)] = HeadButton(WidgetTree, Row, Caption, Width, Align, Margin, RowActionDelegate, Target);
        StationSortCaptions[int32(Column)] = Caption;
    };

    // The tick over the ticks: every requester the table shows, or none of them.
    StationSelectAllCheck = WidgetTree->ConstructWidget<UDTTRowCheck>();
    StationSelectAllCheck->Bind(RowActionDelegate);
    FDTTRowTarget All;
    All.Action = EDTTRowAction::StationSelectAll;
    StationSelectAllCheck->Attach(All);
    StyleCheck(StationSelectAllCheck);
    USizeBox* CheckBox = WidgetTree->ConstructWidget<USizeBox>();
    CheckBox->SetWidthOverride(DTTColCheck);
    CheckBox->SetContent(StationSelectAllCheck);
    Row->AddChild(CheckBox);
    if (UHorizontalBoxSlot* S = Cast<UHorizontalBoxSlot>(CheckBox->Slot)) { S->SetPadding(FMargin(0, 0, 6, 0)); S->SetVerticalAlignment(VAlign_Center); }

    HeadCell(WidgetTree, Row, FString(), DTTColStationTile, HAlign_Left, FMargin(0, 0, 8, 0));
    Sortable(EDTTStationSort::Station, TEXT("STATION"), 0.0f);
    Sortable(EDTTStationSort::Resource, TEXT("RESOURCE"), DTTColItem);
    Sortable(EDTTStationSort::Group, TEXT("GROUP"), DTTColGroup);
    Sortable(EDTTStationSort::Role, TEXT("ROLE"), DTTColRole);
    Sortable(EDTTStationSort::Mode, TEXT("MODE"), DTTColMode);
    Sortable(EDTTStationSort::Stock, TEXT("STOCK"), DTTStockBarWide, HAlign_Left, FMargin(0, 0, 8, 0));
    HeadCell(WidgetTree, Row, FString(), DTTColStock);
    Sortable(EDTTStationSort::Limit, TEXT("LIMIT"), DTTColLimit, HAlign_Right, FMargin(0, 0, 16, 0));
    Sortable(EDTTStationSort::State, TEXT("STATE"), DTTColState);
    Sortable(EDTTStationSort::Wait, TEXT("WAIT"), DTTColWait, HAlign_Right);
    HeadCell(WidgetTree, Row, FString(), DTTColChevron);
}

void UDynamicTimeTableWidget::BuildStationFilters(UHorizontalBox* Row)
{
    if (!Row) return;
    // One chip per role, each with its count; the one in force is filled. Built here once and
    // restyled by every refresh, like the captions.
    StationChipButtons.Reset();
    StationChipCounts.Reset();
    for (int32 Index = 0; Index < int32(EDTTStationRoleFilter::Count); ++Index)
    {
        UDTTRowButton* Chip = WidgetTree->ConstructWidget<UDTTRowButton>();
        Chip->Bind(RowActionDelegate);
        FDTTRowTarget Target;
        Target.Action = EDTTRowAction::StationFilter;
        Target.Value = Index;
        Chip->Attach(Target);
        StyleChip(Chip, Index == int32(StationRoleFilter));
        UHorizontalBox* Inside = WidgetTree->ConstructWidget<UHorizontalBox>();
        Chip->SetContent(Inside);
        AddRowItem(Inside, MakeText(WidgetTree, RoleChipLabels[Index], 12, Hex(0xdce8ee)), 0)->SetPadding(FMargin(0, 0, 6, 0));
        UTextBlock* Count = MakeText(WidgetTree, TEXT("0"), 11, Hex(0x9fb2bd));
        AddRowItem(Inside, Count, 0);
        AddRowItem(Row, Chip, 0)->SetPadding(FMargin(0, 0, 6, 0));
        StationChipButtons.Add(Chip);
        StationChipCounts.Add(Count);
    }
    AddRowItem(Row, MakeText(WidgetTree, FString(), 12, DTTGrey), 0, true);

    // The resource: one of the resources the table has, or all of them. Its options follow the
    // world and are rebuilt only when the set changes, or the box would drop its choice.
    StationResourceCombo = WidgetTree->ConstructWidget<UComboBoxString>();
    StyleCombo(StationResourceCombo, 13);
    StationResourceCombo->AddOption(AllResourcesOption);
    StationResourceCombo->SetSelectedOption(AllResourcesOption);
    StationResourceCombo->OnSelectionChanged.AddDynamic(this, &UDynamicTimeTableWidget::HandleStationResourceChanged);
    USizeBox* ComboBox = WidgetTree->ConstructWidget<USizeBox>();
    ComboBox->SetWidthOverride(230.0f);
    ComboBox->SetContent(StationResourceCombo);
    AddRowItem(Row, ComboBox, 0);
}

void UDynamicTimeTableWidget::HandleStationResourceChanged(FString SelectedItem, ESelectInfo::Type SelectionType)
{
    if (bRebuildingStationResources) return;
    StationResourceFilter = SelectedItem == AllResourcesOption ? FString() : SelectedItem;
    RefreshStationDisplay();
}

void UDynamicTimeTableWidget::BuildStationTools(UHorizontalBox* Row)
{
    if (!Row) return;
    StationSelectionText = MakeText(WidgetTree, TEXT("Nothing selected"), 13, DTTGrey);
    AddRowItem(Row, StationSelectionText, 4, true);

    AddRowItem(Row, MakeText(WidgetTree, TEXT("Limit"), 13, DTTGrey), 4);
    StationLimitInput = WidgetTree->ConstructWidget<USpinBox>();
    StationLimitInput->SetMinValue(0); StationLimitInput->SetMaxValue(9999);
    StationLimitInput->SetMinSliderValue(0); StationLimitInput->SetMaxSliderValue(200);
    StationLimitInput->SetDelta(1); StationLimitInput->SetValue(0);
    // A limit is stacks, and a spin box that offers 0.0 invites a number nobody can set.
    StationLimitInput->SetMinFractionalDigits(0); StationLimitInput->SetMaxFractionalDigits(0);
    StationLimitInput->SetMinDesiredWidth(90);
    StyleSpin(StationLimitInput);
    AddRowItem(Row, StationLimitInput, 4);

    StationApplyButton = MakeButton(WidgetTree, TEXT("Set the limit"));
    StationApplyButton->OnClicked.AddDynamic(this, &UDynamicTimeTableWidget::HandleStationApplyClicked);
    AddRowItem(Row, StationApplyButton, 4);
    StationClearButton = MakeButton(WidgetTree, TEXT("Clear the selection"));
    StationClearButton->OnClicked.AddDynamic(this, &UDynamicTimeTableWidget::HandleStationClearClicked);
    AddRowItem(Row, StationClearButton, 4);
}

void UDynamicTimeTableWidget::GatherStationRows(TArray<FDTTStationRow>& OutRows) const
{
    OutRows.Reset();
    if (IsRemoteClient())
    {
        if (RemoteCallObject) OutRows = RemoteCallObject->GetSnapshot().StationRows;
        return;
    }
    if (UDynamicTimeTableSubsystem* Subsystem = GetWorld() ? GetWorld()->GetSubsystem<UDynamicTimeTableSubsystem>() : nullptr)
    {
        Subsystem->BuildStationRows(OutRows);
    }
}

void UDynamicTimeTableWidget::GatherSelectedDocks(TArray<AFGDockingStationIdentifier*>& OutDocks) const
{
    OutDocks.Reset();
    TArray<FDTTStationRow> Rows;
    GatherStationRows(Rows);
    for (const FDTTStationRow& Row : Rows)
    {
        if (!SelectedStations.Contains(Row.DockingPathNodeGuid)) continue;
        if (AFGDockingStationIdentifier* Dock = Row.Dock.Get()) OutDocks.Add(Dock);
    }
}

void UDynamicTimeTableWidget::HandleStationClearClicked()
{
    SelectedStations.Reset();
    SetEditorStatus(TEXT("dTT: selection cleared."));
    RefreshStationDisplay();
}

void UDynamicTimeTableWidget::HandleStationApplyClicked()
{
    TArray<AFGDockingStationIdentifier*> Docks;
    GatherSelectedDocks(Docks);
    if (Docks.Num() == 0)
    {
        SetEditorStatus(TEXT("dTT: tick the stations to set the limit on."), true);
        return;
    }
    const int32 Limit = StationLimitInput ? FMath::RoundToInt(StationLimitInput->GetValue()) : 0;
    // Renaming real stations, and more than three of them is no longer an edit anybody can undo
    // by eye, so that asks twice. The arming lives in the same pair as the row actions.
    if (Docks.Num() > 3 && !IsArmed(EDTTRowAction::StationSelect, TEXT("bulk")))
    {
        ArmedAction = EDTTRowAction::StationSelect;
        ArmedKey = TEXT("bulk");
        SetEditorStatus(FString::Printf(TEXT("dTT: click again to rename %d stations."), Docks.Num()));
        return;
    }
    ArmedAction = EDTTRowAction::None;
    ArmedKey.Reset();

    if (IsRemoteClient())
    {
        if (!RemoteCallObject) { SetEditorStatus(TEXT("dTT: no connection to the server."), true); return; }
        RemoteCallObject->RequestSetDockTriggerMany(Docks, Limit);
        SetEditorStatus(TEXT("dTT: asked the server..."));
        return;
    }
    UDynamicTimeTableSubsystem* Subsystem = GetWorld() ? GetWorld()->GetSubsystem<UDynamicTimeTableSubsystem>() : nullptr;
    if (!Subsystem) { SetEditorStatus(TEXT("dTT: subsystem unavailable."), true); return; }
    FString Message;
    const bool bOk = Subsystem->SetDockThresholdMany(Docks, Limit, Message);
    SetEditorStatus(Message, !bOk);
    RefreshStationDisplay();
}

void UDynamicTimeTableWidget::RefreshStationDisplay()
{
    TArray<FDTTStationRow> Rows;
    GatherStationRows(Rows);

    // The resource box follows the world: rebuilt when the set of resources changes, and only then.
    {
        TArray<FString> Resources;
        for (const FDTTStationRow& Row : Rows) if (!Row.ResourceId.IsEmpty()) Resources.AddUnique(Row.ResourceId);
        Resources.Sort([](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::IgnoreCase) < 0; });
        if (StationResourceCombo && Resources != StationResourceOptions)
        {
            StationResourceOptions = Resources;
            if (!StationResourceFilter.IsEmpty() && !Resources.Contains(StationResourceFilter)) StationResourceFilter.Reset();
            TGuardValue<bool> Guard(bRebuildingStationResources, true);
            StationResourceCombo->ClearOptions();
            StationResourceCombo->AddOption(AllResourcesOption);
            for (const FString& Resource : Resources) StationResourceCombo->AddOption(Resource);
            StationResourceCombo->SetSelectedOption(StationResourceFilter.IsEmpty() ? FString(AllResourcesOption) : StationResourceFilter);
        }
    }

    const FString Filter = StationFilter ? StationFilter->GetText().ToString().TrimStartAndEnd() : FString();
    TArray<const FDTTStationRow*> Shown;
    int32 Requesters = 0, Open = 0;
    int32 ChipCounts[int32(EDTTStationRoleFilter::Count)] = {};
    for (const FDTTStationRow& Row : Rows)
    {
        if (Row.Role == EDTTDispatchRole::Requester) ++Requesters;
        if (Row.Flags & EDTTStationFlags::Open) ++Open;
        if (!Filter.IsEmpty() && !Row.Name.Contains(Filter) && !Row.ResourceId.Contains(Filter) && !Row.GroupName.Contains(Filter)) continue;
        if (!StationResourceFilter.IsEmpty() && !Row.ResourceId.Equals(StationResourceFilter, ESearchCase::IgnoreCase)) continue;
        // The chips count what the words and the resource leave, so a chip says how many rows it
        // would show.
        for (int32 Chip = 0; Chip < int32(EDTTStationRoleFilter::Count); ++Chip)
        {
            if (MatchesRole(Row, EDTTStationRoleFilter(Chip))) ++ChipCounts[Chip];
        }
        if (!MatchesRole(Row, StationRoleFilter)) continue;
        Shown.Add(&Row);
    }
    for (int32 Chip = 0; Chip < StationChipButtons.Num(); ++Chip)
    {
        StyleChip(StationChipButtons[Chip], Chip == int32(StationRoleFilter));
        if (StationChipCounts.IsValidIndex(Chip) && StationChipCounts[Chip]) StationChipCounts[Chip]->SetText(FText::AsNumber(ChipCounts[Chip]));
    }

    if (StationSort != EDTTStationSort::None)
    {
        const int32 OverdueMinutes = ViewSettings().OverdueRequestMinutes;
        auto Fraction = [](const FDTTStationRow& R) { return (R.StockStacks >= 0 && R.CapacityStacks > 0) ? float(R.StockStacks) / R.CapacityStacks : -1.0f; };
        auto Compare = [&](const FDTTStationRow& A, const FDTTStationRow& B) -> int32
        {
            switch (StationSort)
            {
            case EDTTStationSort::Station: return A.Name.Compare(B.Name, ESearchCase::IgnoreCase);
            case EDTTStationSort::Resource: return A.ResourceId.Compare(B.ResourceId, ESearchCase::IgnoreCase);
            case EDTTStationSort::Group: return A.GroupName.Compare(B.GroupName, ESearchCase::IgnoreCase);
            case EDTTStationSort::Role: return RoleText(A).Compare(RoleText(B));
            case EDTTStationSort::Mode: return Order(ModeRank(A), ModeRank(B));
            case EDTTStationSort::Stock: return Order(Fraction(A), Fraction(B));
            case EDTTStationSort::Limit: return Order(A.TagThreshold, B.TagThreshold);
            case EDTTStationSort::State: return Order(StateRank(A, OverdueMinutes), StateRank(B, OverdueMinutes));
            case EDTTStationSort::Wait: return Order(A.WaitingSeconds, B.WaitingSeconds);
            default: return 0;
            }
        };
        // A tie falls back to the name, whichever way the column runs, so equal rows keep still.
        Algo::StableSort(Shown, [&](const FDTTStationRow* A, const FDTTStationRow* B)
        {
            int32 Result = Compare(*A, *B);
            if (bStationSortDescending) Result = -Result;
            if (Result != 0) return Result < 0;
            return A->Name.Compare(B->Name, ESearchCase::IgnoreCase) < 0;
        });
    }
    for (int32 Column = 0; Column < StationSortLabels.Num(); ++Column)
    {
        UTextBlock* Label = StationSortLabels[Column].Get();
        if (!Label || !StationSortCaptions.IsValidIndex(Column)) continue;
        const bool bOn = Column == int32(StationSort);
        Label->SetText(FText::FromString(bOn
            ? StationSortCaptions[Column] + TEXT(" ") + FString::Chr(bStationSortDescending ? 0x25BC : 0x25B2)
            : StationSortCaptions[Column]));
        Label->SetColorAndOpacity(FSlateColor(bOn ? DTTCyan : FLinearColor(0.72f, 0.78f, 0.82f)));
    }

    ShownStationRequesters.Reset();
    for (const FDTTStationRow* Row : Shown) if (Row->Role == EDTTDispatchRole::Requester) ShownStationRequesters.Add(Row->DockingPathNodeGuid);
    if (StationSelectAllCheck)
    {
        bool bAll = ShownStationRequesters.Num() > 0;
        for (const FGuid& Guid : ShownStationRequesters) if (!SelectedStations.Contains(Guid)) { bAll = false; break; }
        if (StationSelectAllCheck->IsChecked() != bAll) StationSelectAllCheck->SetIsChecked(bAll);
        StationSelectAllCheck->SetVisibility(ShownStationRequesters.Num() > 0 ? ESlateVisibility::Visible : ESlateVisibility::Hidden);
    }

    // The tick is deliberately not in the key: a ticked row has exactly the shape of an unticked
    // one, and both the box and the row's tint are written on every pass. Only what changes the
    // widget count belongs here - whether the row is open, and what kind of station it is.
    TArray<FString> Keys;
    TArray<bool> Expanded;
    TArray<FString> Ids;
    for (const FDTTStationRow* Row : Shown)
    {
        const FString Id = Row->DockingPathNodeGuid.ToString(EGuidFormats::Short);
        const bool bOpen = ExpandedStationKey == Id;
        Ids.Add(Id);
        Expanded.Add(bOpen);
        Keys.Add(FString::Printf(TEXT("%s|%s|%d|%d"), *Id, bOpen ? TEXT("E") : TEXT("c"),
            int32(Row->Role), (Row->Flags & (EDTTStationFlags::Depot | EDTTStationFlags::FuelStop))));
    }
    if (Shown.Num() == 0) Keys.Add(TEXT("empty"));

    {
        int32 Wrong = 0;
        for (const FDTTStationRow& Row : Rows) if ((Row.Flags & EDTTStationFlags::ModeKnown) && !(Row.Flags & EDTTStationFlags::ModeMatches) && !(Row.Flags & (EDTTStationFlags::Depot | EDTTStationFlags::FuelStop))) ++Wrong;
        FDTTStatusLine Line;
        Line.Count(Rows.Num(), TEXT("docks")).Count(Requesters, TEXT("requesters")).Count(Open, TEXT("open"), DTTYellow);
        if (Wrong > 0) Line.Count(Wrong, TEXT("with a mode that fights the tag"), DTTRed);
        if (!Filter.IsEmpty() || !StationResourceFilter.IsEmpty() || StationRoleFilter != EDTTStationRoleFilter::All)
        {
            Line.Count(Shown.Num(), TEXT("shown"), DTTCyan);
        }
        SetStatusLine(Line, FString());
    }
    if (StationSelectionText)
    {
        StationSelectionText->SetText(FText::FromString(SelectedStations.Num() == 0
            ? FString(TEXT("Nothing selected - tick the stations to edit"))
            : FString::Printf(TEXT("%d selected"), SelectedStations.Num())));
        StationSelectionText->SetColorAndOpacity(FSlateColor(SelectedStations.Num() == 0 ? DTTGrey : DTTCyan));
    }

    SyncCards(StationScrollBox, StationCards, Keys, [&](int32 Index, FDTTCardSink& Sink)
    {
        if (!Shown.IsValidIndex(Index))
        {
            UVerticalBox* Body = Sink.Root(DTTCardTint);
            DTTCardHeader(Sink, Body, Rows.Num() == 0 ? TEXT("No docking stations") : TEXT("Nothing matches the filter"), DTTGrey, FString());
            return;
        }
        PopulateStationCard(*Shown[Index], Ids[Index], Expanded[Index], Sink);
    });
}

void UDynamicTimeTableWidget::PopulateStationCard(const FDTTStationRow& Row, const FString& CardId, bool bExpanded, FDTTCardSink& Sink)
{
    const bool bRequester = Row.Role == EDTTDispatchRole::Requester;
    const bool bOpen = (Row.Flags & EDTTStationFlags::Open) != 0;
    const bool bMatches = (Row.Flags & EDTTStationFlags::ModeMatches) != 0;
    const bool bDepot = (Row.Flags & EDTTStationFlags::Depot) != 0;
    const bool bFuel = (Row.Flags & EDTTStationFlags::FuelStop) != 0;
    const int32 OverdueMinutes = ViewSettings().OverdueRequestMinutes;
    const bool bOverdue = OverdueMinutes > 0 && bOpen && Row.ClaimOwner.IsEmpty() && Row.WaitingSeconds >= OverdueMinutes * 60;
    const bool bSelected = SelectedStations.Contains(Row.DockingPathNodeGuid);

    FLinearColor Tint = DTTCardTint;
    if (bOverdue) Tint = DTTCardTintWarn;
    else if (bOpen && Row.ClaimOwner.IsEmpty()) Tint = DTTCardTintWait;
    else if (bOpen) Tint = DTTCardTintOpen;
    UVerticalBox* Body = Sink.Root(bExpanded || bSelected ? Lighten(Tint) : Tint, FMargin(0));

    FDTTRowTarget Toggle;
    Toggle.Action = EDTTRowAction::ToggleExpandStation;
    Toggle.CardKey = CardId;
    Toggle.Guid = Row.DockingPathNodeGuid;
    Toggle.Dock = Row.Dock.Get();
    UHorizontalBox* Line = Sink.Row(Body, DTTRowHeight, Toggle, FLinearColor::White);

    FDTTRowTarget Select = Toggle;
    Select.Action = EDTTRowAction::StationSelect;
    // Only a requester can carry a limit, so only a requester can be ticked for the limit box.
    Sink.Check(Line, bSelected, Select, bRequester, DTTColCheck);
    Sink.Tile(Line, IconFor(Row.ItemClass), bDepot ? DTTSteel : (bFuel ? DTTOrange : TileColorFor(Row.ItemClass)), DTTColStationTile);
    Sink.Cell(Line, DTTStationName(Row.Name), 14, bMatches ? DTTText : DTTRed, 0.0f);
    Sink.Cell(Line, Row.ResourceId, 13, DTTCyan, DTTColItem);
    Sink.Cell(Line, Row.GroupName, 12, DTTSteel, DTTColGroup);
    Sink.Pill(Line, RoleText(Row), RolePill(Row), DTTColRole);

    const TCHAR* Mode = !(Row.Flags & EDTTStationFlags::ModeKnown) ? TEXT("?")
        : ((Row.Flags & EDTTStationFlags::LoadMode) ? TEXT("LOAD") : TEXT("UNLOAD"));
    Sink.Pill(Line, Mode, bMatches ? EDTTPill::Mode : EDTTPill::ModeWrong, DTTColMode);

    FLinearColor BarColor = DTTStocked;
    if (!bMatches) BarColor = DTTDim;
    else if (bOverdue) BarColor = DTTRed;
    else if (bOpen) BarColor = Row.ClaimOwner.IsEmpty() ? DTTYellow : DTTGreen;
    else if (Row.Role == EDTTDispatchRole::Producer) BarColor = DTTCyan;
    Sink.MarkedBar(Line, StationLevel(Row.StockStacks, Row.CapacityStacks),
        StationLevel(Row.TriggerStacks, Row.CapacityStacks),
        StationLevel(Row.ThresholdStacks, Row.CapacityStacks),
        BarColor, DTTStockBarWide);
    Sink.Cell(Line, Row.StockStacks >= 0
        ? (Row.CapacityStacks > 0 ? FString::Printf(TEXT("%d/%d"), Row.StockStacks, Row.CapacityStacks) : FString::Printf(TEXT("%d"), Row.StockStacks))
        : FString(TEXT("-")), 12, DTTGrey, DTTColStock);

    // The limit as the name spells it, and a dash when it says nothing: the number in the tag is
    // what the box writes, so it is the number the table has to show.
    Sink.Cell(Line, bRequester ? (Row.TagThreshold > 0 ? FString::Printf(TEXT("%d"), Row.TagThreshold) : FString(TEXT("-"))) : FString(),
        12, Row.TagThreshold > 0 ? FLinearColor::White : DTTDim, DTTColLimit, HAlign_Right, FMargin(0, 0, 16, 0));

    FString State;
    FLinearColor StateColor = DTTDim;
    if (!bMatches) { State = (Row.Flags & EDTTStationFlags::LoadMode) ? TEXT("wrong mode: LOAD") : TEXT("wrong mode: UNLOAD"); StateColor = DTTRed; }
    else if (bOverdue) { State = TEXT("overdue"); StateColor = DTTRed; }
    else if (bOpen && Row.ClaimOwner.IsEmpty()) { State = TEXT("open, nobody took it"); StateColor = DTTYellow; }
    else if (bOpen) { State = FString::Printf(TEXT("open, %s"), *DTTTruckTitle(Row.ClaimOwner)); StateColor = DTTGreen; }
    else if (!Row.ClaimOwner.IsEmpty()) { State = FString::Printf(TEXT("held by %s"), *DTTTruckTitle(Row.ClaimOwner)); StateColor = DTTGrey; }
    else if (bDepot) State = TEXT("free");
    else if (Row.Role == EDTTDispatchRole::Producer) State = TEXT("producing");
    else if (bRequester) State = TEXT("above its limit");
    if (Row.Flags & EDTTStationFlags::Stay) State = State.IsEmpty() ? TEXT("keeps the truck") : State + TEXT(" = ");
    Sink.Cell(Line, State, 12, StateColor, DTTColState);
    Sink.Cell(Line, StationWaitText(Row.WaitingSeconds), 12, bOverdue ? DTTRed : DTTGrey, DTTColWait, HAlign_Right);
    Sink.Cell(Line, bExpanded ? TEXT("v") : TEXT(">"), 13, DTTDim, DTTColChevron, HAlign_Center);

    if (!bExpanded) return;

    UHorizontalBox* Columns = Sink.HBox(Body, FMargin(10, 4, 10, 8));
    UVerticalBox* Left = Sink.Column(Columns);
    UVerticalBox* Right = Sink.Column(Columns);

    Sink.Text(Left, Row.ResourceId.IsEmpty() ? TEXT("no resource tag") : FString::Printf(TEXT("Resource: %s"), *Row.ResourceId), 13, DTTGrey, true);
    FString Levels = Row.StockStacks >= 0
        ? (Row.CapacityStacks > 0 ? FString::Printf(TEXT("stock %d / %d stacks"), Row.StockStacks, Row.CapacityStacks) : FString::Printf(TEXT("stock %d stacks"), Row.StockStacks))
        : FString(TEXT("stock unknown"));
    if (Row.TriggerStacks >= 0)
    {
        Levels += (Row.Flags & EDTTStationFlags::Fluid) && Row.CapacityStacks > 0
            ? FString::Printf(TEXT("  -  calls below %d (%d%%)"), Row.TriggerStacks, FMath::RoundToInt(100.0f * Row.TriggerStacks / Row.CapacityStacks))
            : FString::Printf(TEXT("  -  calls below %d"), Row.TriggerStacks);
    }
    if (Row.ThresholdStacks >= 0) Levels += FString::Printf(TEXT("  -  fills to %d"), Row.ThresholdStacks);
    Sink.Text(Left, Levels, 12, DTTGrey, true);
    Sink.Text(Left, Row.TagThreshold > 0
        ? FString::Printf(TEXT("The name sets the limit to %d. Tick the row and use the box above to change it."), Row.TagThreshold)
        : (bRequester ? FString(TEXT("The name sets no limit, so the one in the settings applies.")) : FString()), 12, DTTDim, true);
    Sink.Text(Left, Row.GroupName.IsEmpty() ? FString() : FString::Printf(TEXT("Group: %s"), *Row.GroupName), 12, DTTSteel, true);

    Sink.Text(Right, Row.ClaimOwner.IsEmpty() ? TEXT("No truck is holding this station.") : FString::Printf(TEXT("Held by %s"), *Row.ClaimOwner), 13, DTTGrey, true);
    Sink.Text(Right, (Row.Flags & EDTTStationFlags::Stay)
        ? TEXT("The truck stays here until its cargo is gone (the \"=\" in the name).")
        : FString(), 12, DTTSteel, true);

    // Every button is emitted on every pass; what it cannot do right now is greyed, never left
    // out - see FDTTCardSink::Button.
    UHorizontalBox* Buttons = Sink.HBox(Right, FMargin(0, 6, 0, 0));
    const bool bStay = (Row.Flags & EDTTStationFlags::Stay) != 0;
    FDTTRowTarget Target;
    Target.CardKey = CardId;
    Target.Guid = Row.DockingPathNodeGuid;
    Target.Dock = Row.Dock.Get();
    const bool bHasDock = Row.Dock != nullptr;

    Target.Action = bStay ? EDTTRowAction::StationStayOff : EDTTRowAction::StationStayOn;
    Sink.Button(Buttons, bStay ? TEXT("Do not keep the truck") : TEXT("Keep the truck until empty"), Target, true, bHasDock && bRequester);
    Target.Action = EDTTRowAction::StationClearClaim;
    Sink.Button(Buttons, TEXT("Drop the claim"), Target, true, bHasDock && !Row.ClaimOwner.IsEmpty());
    Target.Action = EDTTRowAction::StationSendNow;
    Sink.Button(Buttons, TEXT("Ask for a truck now"), Target, true, bHasDock && bOpen && Row.ClaimOwner.IsEmpty());
}
