#include "DTTCardSink.h"

#include "DynamicTimeTableSubsystem.h"
#include "DynamicTimeTableRemoteCallObject.h"
#include "Resources/FGItemDescriptor.h"
#include "Components/Border.h"
#include "Components/BorderSlot.h"
#include "Components/EditableTextBox.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/ScrollBox.h"
#include "Components/ScrollBoxSlot.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"

using namespace DTTUI;

namespace
{
    /** Stops of a lap drawn across its row; the rest of the lap is a "+N". */
    constexpr int32 MaxLapStops = 5;
    /** What one stop needs before another is let into the row: a name and a leg after it. */
    constexpr float StopNameMin = 90.0f;
    constexpr float StopLegMin = 40.0f;
    /** Segments of one figure's bar. */
    constexpr int32 KpiSegmentCount = 4;

    using FSegment = TPair<float, FLinearColor>;

    FString Dot() { return FString(TEXT(" ")) + FString::Chr(0x00B7) + TEXT(" "); }

    FString Clock(int32 Seconds)
    {
        return FString::Printf(TEXT("%d:%02d"), Seconds / 60, Seconds % 60);
    }

    /**
     * Metres while they are metres, kilometres once they stop being readable, and always with a
     * "~": the first leg is a straight line and the rest is road, so this is an estimate and says so.
     */
    FString DistanceText(int32 Metres)
    {
        if (Metres < 0) return TEXT("-");
        if (Metres < 1000) return FString::Printf(TEXT("~%d m"), Metres);
        return FString::Printf(TEXT("~%.1f km"), Metres / 1000.0f);
    }

    FString SpeedText(int32 Kmh)
    {
        return Kmh < 0 ? FString(TEXT("-")) : FString::Printf(TEXT("%d km/h"), Kmh);
    }

    /**
     * The time to the stop the truck is heading for, as the server worked it out: the road left on
     * this leg over the last minute of driving. Nothing for a truck that is not on the road.
     */
    FString NextStopText(const FDTTLapRow& Row)
    {
        if (!Row.bAutopilot || Row.bDocking || Row.Phase == EDTTTruckPhase::Parked) return TEXT("-");
        if (Row.NextStopSeconds < 0) return TEXT("-");
        if (Row.NextStopSeconds >= 3600) return TEXT(">1 h");
        return TEXT("~") + Clock(Row.NextStopSeconds);
    }

    FString PhaseText(EDTTTruckPhase Phase, bool bDocking)
    {
        if (bDocking) return TEXT("docking");
        switch (Phase)
        {
        case EDTTTruckPhase::ToDepot: return TEXT("pool - to a depot");
        case EDTTTruckPhase::Parked: return TEXT("pool - parked");
        case EDTTTruckPhase::Assigned: return TEXT("pool - on a lap");
        case EDTTTruckPhase::Manual: return TEXT("manual");
        default: return TEXT("dispatch");
        }
    }

    /** The chip's words: short, because six of them share a pane a third of the window wide. */
    FString ReasonText(EDTTSkipReason Reason)
    {
        switch (Reason)
        {
        case EDTTSkipReason::ItemClassNotFound: return TEXT("tag names no item");
        case EDTTSkipReason::Form: return TEXT("solid against fluid");
        case EDTTSkipReason::ForeignCargo: return TEXT("foreign cargo");
        case EDTTSkipReason::NoCargo: return TEXT("nothing on board");
        case EDTTSkipReason::Unreachable: return TEXT("no road to it");
        case EDTTSkipReason::NoProducerWithStock: return TEXT("producer empty");
        case EDTTSkipReason::NoSuitableTruck: return TEXT("no free truck");
        default: return TEXT("-");
        }
    }

    /** The three groups of the laps pane, in the order they are listed. */
    enum class ELapGroup : uint8 { Attention, Road, Parked, Count };

    ELapGroup GroupOf(const FDTTLapRow& Row)
    {
        if (!Row.bAutopilot) return ELapGroup::Attention;
        if (Row.bDocking || Row.Phase == EDTTTruckPhase::Parked) return ELapGroup::Parked;
        return ELapGroup::Road;
    }

    const TCHAR* GroupTitle(ELapGroup Group)
    {
        switch (Group)
        {
        case ELapGroup::Attention: return TEXT("NEEDS ATTENTION");
        case ELapGroup::Road: return TEXT("ON THE ROAD");
        default: return TEXT("DOCKED / PARKED");
        }
    }

    /** A pane of the Timetable: an outlined panel with a head bar; returns the body under the head. */
    UVerticalBox* MakePane(UWidgetTree* Tree, UHorizontalBox* Split, float Share, const FMargin& Margin,
        const FString& Title, TObjectPtr<UTextBlock>& OutSummary, UHorizontalBox*& OutHead)
    {
        UBorder* Frame = Tree->ConstructWidget<UBorder>();
        Frame->SetBrush(OutlinedBrush(DTTPane, Hex(0x3a4f5b), 10.0f));
        Frame->SetPadding(FMargin(1));
        UHorizontalBoxSlot* FrameSlot = Split->AddChildToHorizontalBox(Frame);
        FSlateChildSize Size(ESlateSizeRule::Fill);
        Size.Value = Share;
        FrameSlot->SetSize(Size);
        FrameSlot->SetPadding(Margin);

        UVerticalBox* Body = Tree->ConstructWidget<UVerticalBox>();
        Frame->SetContent(Body);
        UBorder* Head = Tree->ConstructWidget<UBorder>();
        Head->SetBrush(CornerBrush(DTTPaneHead, FVector4(9, 9, 0, 0)));
        Head->SetPadding(FMargin(14, 0, 10, 0));
        USizeBox* HeadBox = Tree->ConstructWidget<USizeBox>();
        HeadBox->SetHeightOverride(38.0f);
        HeadBox->SetContent(Head);
        Body->AddChildToVerticalBox(HeadBox);
        OutHead = Tree->ConstructWidget<UHorizontalBox>();
        Head->SetContent(OutHead);
        if (UBorderSlot* S = Cast<UBorderSlot>(OutHead->Slot)) S->SetVerticalAlignment(VAlign_Center);
        AddRowItem(OutHead, MakeText(Tree, Title, 14, DTTText), 0)->SetPadding(FMargin(0, 0, 10, 0));
        OutSummary = MakeText(Tree, FString(), 12, DTTGrey);
        OutSummary->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
        AddRowItem(OutHead, OutSummary, 0, true);
        return Body;
    }

    /** Padding of a pane's scroll box, and the width of its bar with the bar's own padding. */
    constexpr float PaneScrollLeft = 4.0f, PaneScrollRight = 2.0f, PaneScrollBar = 6.0f + 2 * 2.0f;

    UScrollBox* MakePaneScroll(UWidgetTree* Tree, UVerticalBox* Body)
    {
        UScrollBox* Scroll = Tree->ConstructWidget<UScrollBox>();
        // Always there and always the same width, so the right-hand columns of a short list and
        // of a long one stand in the same place under their captions.
        Scroll->SetAlwaysShowScrollbar(true);
        Scroll->SetScrollbarThickness(FVector2D(6.0f, 6.0f));
        Scroll->SetScrollbarPadding(FMargin(2.0f));
        UVerticalBoxSlot* S = Body->AddChildToVerticalBox(Scroll);
        S->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
        S->SetPadding(FMargin(PaneScrollLeft, 2, PaneScrollRight, 4));
        return Scroll;
    }

    /** Everything of a lap row but the lap line itself; see PopulateLapCard and the header. */
    constexpr float LapFixedColumns =
        2 * 2 + 2 * 6                          // the card's slot and the row button's padding
        + DTTColTile + 8 + DTTColLapWho + 8    // tile, who
        + 8                                    // after the lap line
        + DTTColSpeed + 6 + DTTColNext + 6 + DTTColLapLeft + 6 + DTTColLapCargo;
}

UVerticalBox* UDynamicTimeTableWidget::BuildTimetablePage()
{
    UVerticalBox* Page = WidgetTree->ConstructWidget<UVerticalBox>();

    // Four figures over the panes, built here once and rewritten in place by every refresh.
    KpiValues.Reset(); KpiLines.Reset(); KpiSegments.Reset();
    UHorizontalBox* Kpis = WidgetTree->ConstructWidget<UHorizontalBox>();
    USizeBox* KpiBox = WidgetTree->ConstructWidget<USizeBox>();
    KpiBox->SetHeightOverride(84.0f);
    KpiBox->SetContent(Kpis);
    Page->AddChildToVerticalBox(KpiBox)->SetPadding(FMargin(0, 0, 0, 12));
    BuildKpiTile(Kpis, TEXT("OPEN REQUESTS"));
    BuildKpiTile(Kpis, TEXT("WAIT, MEDIAN"));
    BuildKpiTile(Kpis, TEXT("TRUCKS"));
    BuildKpiTile(Kpis, TEXT("BLOCKED BY"));

    UHorizontalBox* Split = WidgetTree->ConstructWidget<UHorizontalBox>();
    Page->AddChildToVerticalBox(Split)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));

    UHorizontalBox* LapsHead = nullptr;
    UVerticalBox* Laps = MakePane(WidgetTree, Split, DTTLapsPaneShare, FMargin(0, 0, 14, 0),
        TEXT("Laps in flight"), LapsPaneSummary, LapsHead);
    TimetableFilter = WidgetTree->ConstructWidget<UEditableTextBox>();
    TimetableFilter->SetHintText(FText::FromString(TEXT("filter: truck, station or resource")));
    StyleInput(TimetableFilter, 12);
    USizeBox* FilterBox = WidgetTree->ConstructWidget<USizeBox>();
    FilterBox->SetWidthOverride(240.0f);
    FilterBox->SetContent(TimetableFilter);
    AddRowItem(LapsHead, FilterBox, 0);
    TimetableFilter->OnTextChanged.AddDynamic(this, &UDynamicTimeTableWidget::HandleTimetableFilterChanged);
    // The captions stay above the scroll box and start where the first column of a row does:
    // the scroll box's padding, the card's slot (2) and the row button's padding (6). On the right
    // the scroll bar comes in between as well.
    UHorizontalBox* Header = WidgetTree->ConstructWidget<UHorizontalBox>();
    Laps->AddChildToVerticalBox(Header)->SetPadding(FMargin(PaneScrollLeft + 2 + 6, 6, PaneScrollRight + PaneScrollBar + 2 + 6, 0));
    BuildTimetableHeader(Header);
    TimetableScrollBox = MakePaneScroll(WidgetTree, Laps);

    UHorizontalBox* QueueHead = nullptr;
    UVerticalBox* Queue = MakePane(WidgetTree, Split, 1.0f, FMargin(0),
        TEXT("Open requests"), QueuePaneSummary, QueueHead);
    QueueScrollBox = MakePaneScroll(WidgetTree, Queue);
    return Page;
}

void UDynamicTimeTableWidget::BuildKpiTile(UHorizontalBox* Row, const FString& Caption)
{
    if (!Row) return;
    const bool bLast = KpiValues.Num() == 3;
    UBorder* Tile = WidgetTree->ConstructWidget<UBorder>();
    Tile->SetBrush(OutlinedBrush(Hex(0x2c3d48), Hex(0x3a4f5b), 9.0f));
    Tile->SetPadding(FMargin(12, 8, 12, 8));
    Tile->SetClipping(EWidgetClipping::ClipToBounds);
    UHorizontalBoxSlot* TileSlot = Row->AddChildToHorizontalBox(Tile);
    TileSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
    TileSlot->SetPadding(FMargin(0, 0, bLast ? 0 : 12, 0));

    UVerticalBox* Stack = WidgetTree->ConstructWidget<UVerticalBox>();
    Tile->SetContent(Stack);
    Stack->AddChildToVerticalBox(MakeText(WidgetTree, Caption, 11, DTTGrey));
    UTextBlock* Value = MakeText(WidgetTree, TEXT("-"), 20, DTTText);
    Value->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
    Stack->AddChildToVerticalBox(Value);
    UTextBlock* Line = MakeText(WidgetTree, FString(), 12, DTTGrey);
    Line->SetTextOverflowPolicy(ETextOverflowPolicy::Ellipsis);
    Stack->AddChildToVerticalBox(Line);

    UHorizontalBox* Bar = WidgetTree->ConstructWidget<UHorizontalBox>();
    USizeBox* BarBox = WidgetTree->ConstructWidget<USizeBox>();
    BarBox->SetHeightOverride(4.0f);
    BarBox->SetContent(Bar);
    Stack->AddChildToVerticalBox(BarBox)->SetPadding(FMargin(0, 4, 0, 0));
    for (int32 Index = 0; Index < KpiSegmentCount; ++Index)
    {
        UBorder* Segment = WidgetTree->ConstructWidget<UBorder>();
        Segment->SetBrush(FlatBrush(FLinearColor::White, 2.0f));
        Segment->SetBrushColor(DTTTrack);
        UHorizontalBoxSlot* S = Bar->AddChildToHorizontalBox(Segment);
        S->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
        S->SetPadding(FMargin(0, 0, 2, 0));
        Segment->SetVisibility(ESlateVisibility::Collapsed);
        KpiSegments.Add(Segment);
    }
    KpiValues.Add(Value);
    KpiLines.Add(Line);
}

void UDynamicTimeTableWidget::SetKpi(int32 Index, const FString& Value, const FLinearColor& ValueColour, const FString& Line,
    const TArray<TPair<float, FLinearColor>>& Segments)
{
    if (KpiValues.IsValidIndex(Index) && KpiValues[Index])
    {
        KpiValues[Index]->SetText(FText::FromString(Value));
        KpiValues[Index]->SetColorAndOpacity(FSlateColor(ValueColour));
    }
    if (KpiLines.IsValidIndex(Index) && KpiLines[Index]) KpiLines[Index]->SetText(FText::FromString(Line));
    for (int32 Part = 0; Part < KpiSegmentCount; ++Part)
    {
        const int32 At = Index * KpiSegmentCount + Part;
        UBorder* Segment = KpiSegments.IsValidIndex(At) ? KpiSegments[At].Get() : nullptr;
        if (!Segment) continue;
        if (!Segments.IsValidIndex(Part) || Segments[Part].Key <= 0.0f)
        {
            Segment->SetVisibility(ESlateVisibility::Collapsed);
            continue;
        }
        Segment->SetBrushColor(Segments[Part].Value);
        Segment->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
        if (UHorizontalBoxSlot* S = Cast<UHorizontalBoxSlot>(Segment->Slot))
        {
            FSlateChildSize Size(ESlateSizeRule::Fill);
            Size.Value = Segments[Part].Key;
            S->SetSize(Size);
        }
    }
}

void UDynamicTimeTableWidget::BuildTimetableHeader(UHorizontalBox* Row)
{
    if (!Row) return;
    HeadCell(WidgetTree, Row, FString(), DTTColTile, HAlign_Left, FMargin(0, 0, 8, 0));
    HeadCell(WidgetTree, Row, TEXT("TRUCK"), DTTColLapWho, HAlign_Left, FMargin(0, 0, 8, 0));
    HeadCell(WidgetTree, Row, TEXT("LAP"), 0.0f, HAlign_Left, FMargin(0, 0, 8, 0));
    HeadCell(WidgetTree, Row, TEXT("SPEED"), DTTColSpeed, HAlign_Right);
    HeadCell(WidgetTree, Row, TEXT("NEXT"), DTTColNext, HAlign_Right);
    HeadCell(WidgetTree, Row, TEXT("LEFT"), DTTColLapLeft, HAlign_Right);
    HeadCell(WidgetTree, Row, TEXT("CARGO"), DTTColLapCargo, HAlign_Right, FMargin(0));
}

void UDynamicTimeTableWidget::RefreshTimetableDisplay()
{
    TArray<FDTTLapRow> Laps;
    TArray<FDTTQueueRow> Queue;
    TArray<FDTTReasonRow> Reasons;
    if (IsRemoteClient())
    {
        if (RemoteCallObject)
        {
            const FDTTClientSnapshot& Snapshot = RemoteCallObject->GetSnapshot();
            Laps = Snapshot.LapRows;
            Queue = Snapshot.QueueRows;
            Reasons = Snapshot.ReasonRows;
        }
    }
    else if (UDynamicTimeTableSubsystem* Subsystem = GetWorld() ? GetWorld()->GetSubsystem<UDynamicTimeTableSubsystem>() : nullptr)
    {
        Subsystem->BuildLapRows(Laps);
        Subsystem->BuildQueueRows(Queue);
        Subsystem->GetPoolSkipRows(Reasons);
    }

    const FString Filter = TimetableFilter ? TimetableFilter->GetText().ToString().TrimStartAndEnd() : FString();
    auto Keep = [&Filter](const FString& A, const FString& B)
    {
        return Filter.IsEmpty() || A.Contains(Filter) || B.Contains(Filter);
    };
    auto KeepLap = [&Keep](const FDTTLapRow& Lap)
    {
        if (Keep(Lap.TruckName, Lap.ResourceId)) return true;
        for (const FString& Stop : Lap.RouteStops) if (Keep(Stop, FString())) return true;
        return false;
    };

    // --- The figures: every lap and every request, whatever the filter hides. ---
    const int32 OverdueMinutes = ViewSettings().OverdueRequestMinutes;
    int32 Claimed = 0, Overdue = 0;
    TArray<int32> Waits;
    const FDTTQueueRow* Worst = nullptr;
    for (const FDTTQueueRow& Row : Queue)
    {
        if (!Row.ClaimOwner.IsEmpty()) { ++Claimed; continue; }
        Waits.Add(Row.WaitingSeconds);
        if (OverdueMinutes > 0 && Row.WaitingSeconds >= OverdueMinutes * 60) ++Overdue;
        if (!Worst || Row.WaitingSeconds > Worst->WaitingSeconds) Worst = &Row;
    }
    const int32 Waiting = Waits.Num();
    {
        FString Line = FString::Printf(TEXT("%d claimed%s%d waiting"), Claimed, *Dot(), Waiting);
        if (Overdue > 0) Line += Dot() + FString::Printf(TEXT("%d overdue"), Overdue);
        SetKpi(0, FString::FromInt(Queue.Num()), Queue.Num() > 0 ? DTTText : DTTGreen, Line,
            { FSegment(float(Claimed), DTTGreen), FSegment(float(Waiting - Overdue), DTTYellow), FSegment(float(Overdue), DTTRed),
              FSegment(Queue.Num() == 0 ? 1.0f : 0.0f, DTTTrack) });
    }
    if (Waiting > 0 && Worst)
    {
        Waits.Sort();
        const int32 Median = Waits[Waits.Num() / 2];
        const bool bMedianOverdue = OverdueMinutes > 0 && Median >= OverdueMinutes * 60;
        SetKpi(1, Clock(Median), bMedianOverdue ? DTTRed : DTTText,
            FString::Printf(TEXT("worst %s%s%s"), *Clock(Worst->WaitingSeconds), *Dot(), *DTTStationName(Worst->Name)),
            { FSegment(float(FMath::Max(Median, 1)), bMedianOverdue ? DTTRed : DTTYellow),
              FSegment(float(Worst->WaitingSeconds - Median), DTTTrack) });
    }
    else
    {
        SetKpi(1, TEXT("-"), DTTDim, TEXT("nobody is waiting"), { FSegment(1.0f, DTTTrack) });
    }

    int32 GroupCounts[int32(ELapGroup::Count)] = {};
    for (const FDTTLapRow& Lap : Laps) ++GroupCounts[int32(GroupOf(Lap))];
    const int32 Driving = GroupCounts[int32(ELapGroup::Road)];
    const int32 Standing = GroupCounts[int32(ELapGroup::Parked)];
    const int32 Attention = GroupCounts[int32(ELapGroup::Attention)];
    {
        FString Line = FString::Printf(TEXT("%d driving%s%d docked or parked"), Driving, *Dot(), Standing);
        if (Attention > 0) Line += Dot() + FString::Printf(TEXT("%d autopilot off"), Attention);
        SetKpi(2, FString::FromInt(Laps.Num()), DTTText, Line,
            { FSegment(float(Driving), DTTCyan), FSegment(float(Standing), DTTSteel), FSegment(float(Attention), DTTYellow),
              FSegment(Laps.Num() == 0 ? 1.0f : 0.0f, DTTTrack) });
    }

    // The reasons of the last pass, one per reason whatever the resource: the chips and the figure
    // say what stops the dispatcher, and /dtt why is there for which truck. OpenRequests is reused
    // as the number of resources the reason stopped.
    TArray<FDTTReasonRow> Totals;
    for (const FDTTReasonRow& Row : Reasons)
    {
        if (!Keep(Row.ResourceId, ReasonText(Row.Reason))) continue;
        FDTTReasonRow* Found = Totals.FindByPredicate([&Row](const FDTTReasonRow& T) { return T.Reason == Row.Reason; });
        if (!Found) { Found = &Totals.AddDefaulted_GetRef(); Found->Reason = Row.Reason; Found->OpenRequests = 0; }
        Found->Count += Row.Count;
        Found->OpenRequests += 1;
    }
    Totals.Sort([](const FDTTReasonRow& A, const FDTTReasonRow& B) { return A.Count > B.Count; });
    if (Totals.Num() > 0)
    {
        const FLinearColor Colours[KpiSegmentCount] = { DTTYellow, DTTRed, DTTOrange, DTTSteel };
        TArray<FSegment> Segments;
        for (int32 Index = 0; Index < Totals.Num() && Index < KpiSegmentCount; ++Index)
        {
            Segments.Emplace(float(Totals[Index].Count), Colours[Index]);
        }
        SetKpi(3, ReasonText(Totals[0].Reason), DTTYellow,
            FString::Printf(TEXT("%d skip(s) in the last pass%s%d resource(s)"), Totals[0].Count, *Dot(), Totals[0].OpenRequests),
            Segments);
    }
    else
    {
        SetKpi(3, TEXT("nothing"), DTTGreen, TEXT("the last pass placed all it could"), { FSegment(1.0f, DTTTrack) });
    }

    {
        FDTTStatusLine Status;
        Status.Count(Laps.Num(), TEXT("laps")).Count(Driving, TEXT("driving"), DTTCyan)
            .Count(Queue.Num(), TEXT("open requests"), DTTYellow);
        if (Overdue > 0) Status.Count(Overdue, TEXT("overdue"), DTTRed);
        if (Attention > 0) Status.Count(Attention, TEXT("autopilot off"), DTTYellow);
        SetStatusLine(Status, FString());
    }
    if (LapsPaneSummary)
    {
        FString Summary = FString::Printf(TEXT("%d on the road"), Driving);
        if (Attention > 0) Summary += Dot() + FString::Printf(TEXT("%d need you"), Attention);
        LapsPaneSummary->SetText(FText::FromString(Summary));
    }
    if (QueuePaneSummary)
    {
        QueuePaneSummary->SetText(FText::FromString(FString::Printf(TEXT("by wait%s%d waiting%s%d taken"), *Dot(), Waiting, *Dot(), Claimed)));
    }

    // --- The laps pane: three groups, a heading over each that has anyone in it. ---
    // The lap line gets what the fixed columns leave, measured from the scroll box once it has
    // been laid out and worked out from the window before that.
    float Scroll = TimetableScrollBox ? TimetableScrollBox->GetCachedGeometry().GetLocalSize().X : 0.0f;
    if (Scroll < 1.0f)
    {
        Scroll = (AppliedWindowSize.X - 2 - 32 - 14) * DTTLapsPaneShare / (DTTLapsPaneShare + 1.0f) - 2 - PaneScrollLeft - PaneScrollRight;
    }
    const float LapWidth = FMath::Max(60.0f, Scroll - PaneScrollBar - LapFixedColumns);

    struct FEntry { int32 Kind = 0; int32 Index = 0; };
    TArray<FEntry> Entries;
    TArray<FString> Keys;
    int32 Shown[int32(ELapGroup::Count)] = {};
    for (int32 Group = 0; Group < int32(ELapGroup::Count); ++Group)
    {
        TArray<int32> Members;
        for (int32 Index = 0; Index < Laps.Num(); ++Index)
        {
            if (int32(GroupOf(Laps[Index])) == Group && KeepLap(Laps[Index])) Members.Add(Index);
        }
        Shown[Group] = Members.Num();
        if (Members.Num() == 0) continue;
        Entries.Add(FEntry{ 1, Group });
        Keys.Add(FString::Printf(TEXT("g|%d"), Group));
        for (int32 Index : Members)
        {
            Entries.Add(FEntry{ 0, Index });
            Keys.Add(FString::Printf(TEXT("l|%s"), *DTTTruckKey(Laps[Index].TruckName)));
        }
    }
    if (Entries.Num() == 0)
    {
        Entries.Add(FEntry{ 2, 0 });
        Keys.Add(TEXT("e"));
    }
    SyncCards(TimetableScrollBox, TimetableCards, Keys, [&](int32 Index, FDTTCardSink& Sink)
    {
        const FEntry& Entry = Entries[Index];
        if (Entry.Kind == 0) { PopulateLapCard(Laps[Entry.Index], LapWidth, Sink); return; }
        if (Entry.Kind == 1)
        {
            const ELapGroup Group = ELapGroup(Entry.Index);
            PopulateSectionCard(GroupTitle(Group), Group == ELapGroup::Attention ? DTTYellow : DTTGrey, FString::FromInt(Shown[Entry.Index]), Sink);
            return;
        }
        PopulateSectionCard(Laps.Num() == 0 ? TEXT("NO TRUCK IS ON A LAP") : TEXT("NOTHING MATCHES THE FILTER"), DTTDim, FString(), Sink);
    });

    // --- The requests pane: the queue in the dispatcher's own order, then why nothing moves. ---
    TArray<FEntry> QueueEntries;
    TArray<FString> QueueKeys;
    for (int32 Index = 0; Index < Queue.Num(); ++Index)
    {
        if (!Keep(Queue[Index].Name, Queue[Index].ResourceId)) continue;
        QueueEntries.Add(FEntry{ 0, Index });
        QueueKeys.Add(FString::Printf(TEXT("q|%s"), *Queue[Index].DockingPathNodeGuid.ToString(EGuidFormats::Short)));
    }
    if (QueueEntries.Num() == 0)
    {
        QueueEntries.Add(FEntry{ 2, 0 });
        QueueKeys.Add(TEXT("e"));
    }
    QueueEntries.Add(FEntry{ 1, 0 });
    QueueKeys.Add(FString::Printf(TEXT("why|%d"), Totals.Num()));
    SyncCards(QueueScrollBox, QueueCards, QueueKeys, [&](int32 Index, FDTTCardSink& Sink)
    {
        const FEntry& Entry = QueueEntries[Index];
        if (Entry.Kind == 0) { PopulateQueueCard(Queue[Entry.Index], Sink); return; }
        if (Entry.Kind == 1) { PopulateReasonCard(Totals, Sink); return; }
        PopulateSectionCard(Queue.Num() == 0 ? TEXT("NOBODY IS ASKING") : TEXT("NOTHING MATCHES THE FILTER"), DTTDim, FString(), Sink);
    });
}

void UDynamicTimeTableWidget::PopulateSectionCard(const FString& Title, const FLinearColor& Colour, const FString& Count, FDTTCardSink& Sink)
{
    UVerticalBox* Body = Sink.Root(FLinearColor(0, 0, 0, 0), FMargin(8, 8, 8, 2));
    UHorizontalBox* Line = Sink.HBox(Body);
    Sink.Text(Line, Title, 11, Colour);
    Sink.Rule(Line, Hex(0x3a4f5b));
    Sink.Text(Line, Count, 11, DTTGrey);
}

void UDynamicTimeTableWidget::PopulateLapCard(const FDTTLapRow& Row, float LapWidth, FDTTCardSink& Sink)
{
    const bool bAttention = !Row.bAutopilot;
    const bool bMoving = Row.bAutopilot && !Row.bDocking && Row.Phase != EDTTTruckPhase::Parked;
    UVerticalBox* Body = Sink.Root(bAttention ? DTTCardTintWait : DTTCardTint, FMargin(0));

    FDTTRowTarget None;
    UHorizontalBox* Line = Sink.Row(Body, DTTRowHeight, None, FLinearColor::White);
    Sink.Tile(Line, IconFor(Row.ItemClass), TileColorFor(Row.ItemClass), DTTColTile);

    // Who: the resource over where the truck is going. The mod names a truck "<resource> -> <dock>",
    // so the name splits into the two lines; a name the player chose stays whole on top.
    const FString Title = DTTTruckTitle(Row.TruckName);
    FString Top = Title, Bottom = PhaseText(Row.Phase, Row.bDocking);
    if (!Row.ResourceId.IsEmpty() && Title.StartsWith(Row.ResourceId, ESearchCase::CaseSensitive))
    {
        Top = Row.ResourceId;
        const FString Rest = Title.Mid(Row.ResourceId.Len()).TrimStart();
        if (!Rest.IsEmpty()) Bottom = Rest;
    }
    else if (!Row.ResourceId.IsEmpty())
    {
        Bottom = Row.ResourceId + Dot() + Bottom;
    }
    if (bAttention) Bottom = TEXT("autopilot is off - the route is kept, nothing is written");
    const bool bPool = Row.Phase == EDTTTruckPhase::Assigned || Row.Phase == EDTTTruckPhase::Parked || Row.Phase == EDTTTruckPhase::ToDepot;
    Sink.Cell2(Line, Top, 13, bAttention ? DTTYellow : DTTText, Bottom, 11, bAttention ? DTTYellow : (bPool ? DTTSteel : DTTGrey), DTTColLapWho);

    // The lap: stop names with the legs between them, the one being driven lit. Repeats of one
    // stop in a row (a pool lap starts with copies of its depot) are one stop.
    TArray<FString> Stops;
    int32 Target = INDEX_NONE;
    for (int32 Index = 0; Index < Row.RouteStops.Num(); ++Index)
    {
        if (Index == 0 || Row.RouteStops[Index] != Row.RouteStops[Index - 1]) Stops.Add(DTTStationName(Row.RouteStops[Index]));
        if (Index == Row.TargetIndex) Target = Stops.Num() - 1;
    }
    // A truck heading for the first stop is on the leg from the last one: the lap is a loop, so
    // the last stop goes in front of the first, behind the truck, and that leg shows.
    int32 StopsLeft = Target == INDEX_NONE ? 0 : Stops.Num() - Target;
    if (Target == 0 && Stops.Num() > 1 && Stops.Last() != Stops[0])
    {
        // A copy first: Insert asserts on an element of the array it is growing.
        const FString Previous = Stops.Last();
        Stops.Insert(Previous, 0);
        Target = 1;
    }
    const int32 Fit = FMath::Clamp(FMath::FloorToInt((LapWidth + StopLegMin) / (StopNameMin + StopLegMin)), 1, MaxLapStops);
    const int32 Visible = FMath::Min(Stops.Num(), Fit);
    // The window over the lap starts one stop behind the target, so the leg being driven shows.
    const int32 First = FMath::Clamp(Target > 0 ? Target - 1 : 0, 0, FMath::Max(0, Stops.Num() - Visible));
    const int32 More = Stops.Num() - (First + Visible);
    const float NameWidth = Visible > 0
        ? FMath::Clamp((LapWidth - (Visible - 1 + (More > 0 ? 1 : 0)) * StopLegMin - (More > 0 ? 24.0f : 0.0f)) / Visible, 48.0f, 170.0f)
        : 170.0f;
    const FLinearColor Now = bAttention ? DTTYellow : (bMoving ? DTTCyan : DTTSteel);
    const FLinearColor Done = Hex(0x9aacb7);
    const FLinearColor DoneLeg(Now.R * 0.45f, Now.G * 0.45f, Now.B * 0.45f, 1.0f);
    const FString Next = NextStopText(Row);

    UHorizontalBox* Lap = Sink.HBox(Line);
    if (Lap)
    {
        Lap->SetClipping(EWidgetClipping::ClipToBounds);
        if (UHorizontalBoxSlot* S = Cast<UHorizontalBoxSlot>(Lap->Slot))
        {
            S->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
            S->SetVerticalAlignment(VAlign_Center);
            S->SetPadding(FMargin(0, 0, 8, 0));
        }
    }
    for (int32 Place = 0; Place < MaxLapStops; ++Place)
    {
        if (Place >= Visible)
        {
            Sink.Stop(Lap, FString(), DTTText, NameWidth, false, DTTTrack);
            continue;
        }
        const int32 At = First + Place;
        const FLinearColor Name = Target == INDEX_NONE ? DTTText : (At < Target ? Done : (At == Target ? Now : DTTText));
        // The leg after this stop leads to At + 1: behind the truck, the one it is on, or ahead.
        const bool bLeg = Place + 1 < Visible || More > 0;
        const bool bCurrent = At + 1 == Target;
        const FLinearColor Leg = Target == INDEX_NONE ? DTTTrack : (At + 1 < Target ? DoneLeg : (bCurrent ? Now : DTTTrack));
        Sink.Stop(Lap, Stops[At], Name, NameWidth, bLeg, Leg, bCurrent && Next != TEXT("-") ? Next : FString());
    }
    Sink.Text(Lap, More > 0 ? FString::Printf(TEXT("+%d"), More) : FString(), 11, DTTDim);

    Sink.Cell(Line, SpeedText(Row.SpeedKmh), 12, Row.SpeedKmh > 0 ? DTTText : DTTDim, DTTColSpeed, HAlign_Right);
    Sink.Cell(Line, Next, 12, Next != TEXT("-") ? DTTCyan : DTTDim, DTTColNext, HAlign_Right);
    Sink.Cell2(Line, DistanceText(Row.DistanceLeftMetres), 12, Row.DistanceLeftMetres > 0 ? DTTText : DTTDim,
        StopsLeft > 1 ? FString::Printf(TEXT("%d stops"), StopsLeft) : (StopsLeft == 1 ? FString(TEXT("last stop")) : FString()), 11, DTTGrey,
        DTTColLapLeft, FMargin(0, 0, 6, 0), HAlign_Right);
    const float Fill = Row.CargoCapacityStacks > 0 ? float(FMath::Max(0, Row.CargoStacks)) / Row.CargoCapacityStacks : 0.0f;
    Sink.CellBar(Line, Row.CargoCapacityStacks > 0
        ? FString::Printf(TEXT("%d / %d st"), FMath::Max(0, Row.CargoStacks), Row.CargoCapacityStacks)
        : FString(TEXT("-")), Row.CargoStacks > 0 ? DTTText : DTTDim, Fill, bAttention ? DTTYellow : DTTCyan,
        DTTColLapCargo, FMargin(0));
}

void UDynamicTimeTableWidget::PopulateQueueCard(const FDTTQueueRow& Row, FDTTCardSink& Sink)
{
    const int32 OverdueMinutes = ViewSettings().OverdueRequestMinutes;
    const bool bTaken = !Row.ClaimOwner.IsEmpty();
    const bool bOverdue = OverdueMinutes > 0 && !bTaken && Row.WaitingSeconds >= OverdueMinutes * 60;
    UVerticalBox* Body = Sink.Root(bOverdue ? DTTCardTintWarn : DTTCardTint, FMargin(0));

    FDTTRowTarget None;
    UHorizontalBox* Line = Sink.Row(Body, DTTRowHeight, None, FLinearColor::White);
    Sink.Tile(Line, IconFor(Row.ItemClass), TileColorFor(Row.ItemClass), 24.0f);
    const FString Stock = FString::Printf(TEXT("%d / %d st"), FMath::Max(0, Row.StockStacks), FMath::Max(0, Row.CapacityStacks));
    const FString About = bTaken
        ? Row.ResourceId + Dot() + Stock + Dot() + DTTTruckTitle(Row.ClaimOwner) + TEXT(" is on the way")
        : Row.ResourceId + Dot() + Stock + Dot() + FString::Printf(TEXT("calls below %d"), FMath::Max(0, Row.TriggerStacks));
    Sink.Cell2(Line, DTTStationName(Row.Name), 13, DTTText, About, 11, DTTGrey, 0.0f);
    Sink.Cell(Line, bTaken ? FString(TEXT("taken")) : Clock(Row.WaitingSeconds), 12,
        bTaken ? DTTGreen : (bOverdue ? DTTRed : DTTYellow), 52.0f, HAlign_Right, FMargin(0));
}

void UDynamicTimeTableWidget::PopulateReasonCard(const TArray<FDTTReasonRow>& Totals, FDTTCardSink& Sink)
{
    UVerticalBox* Body = Sink.Root(Hex(0x263640), FMargin(12, 10));
    Sink.Text(Body, TEXT("WHY NOTHING MOVES"), 11, DTTGrey, false, FMargin(0, 0, 0, 6));
    Sink.Text(Body, Totals.Num() == 0 ? FString(TEXT("The last pass placed every truck it could.")) : FString(), 12, DTTDim);
    UWrapBox* Chips = Sink.Wrap(Body);
    for (const FDTTReasonRow& Row : Totals) Sink.Chip(Chips, ReasonText(Row.Reason), FString::FromInt(Row.Count));
}
