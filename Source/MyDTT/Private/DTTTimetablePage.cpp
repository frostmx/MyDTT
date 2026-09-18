#include "DTTCardSink.h"

#include "DynamicTimeTableSubsystem.h"
#include "DynamicTimeTableRemoteCallObject.h"
#include "Resources/FGItemDescriptor.h"
#include "Components/EditableTextBox.h"
#include "Components/ScrollBox.h"

using namespace DTTUI;

namespace
{
    FString LapWaitText(int32 Seconds)
    {
        return Seconds > 0 ? FString::Printf(TEXT("%d:%02d"), Seconds / 60, Seconds % 60) : FString(TEXT("-"));
    }

    /**
     * Metres while they are metres, kilometres once they stop being readable, and always with a
     * "~": the first leg is a straight line and the rest is road, so this is an estimate and says
     * so. There is deliberately no time here - the game never says what speed a truck will hold.
     */
    FString DistanceText(int32 Metres)
    {
        if (Metres < 0) return TEXT("-");
        if (Metres < 1000) return FString::Printf(TEXT("~%d m"), Metres);
        return FString::Printf(TEXT("~%.1f km"), Metres / 1000.0f);
    }

    FString PhaseText(EDTTTruckPhase Phase, bool bDocking)
    {
        if (bDocking) return TEXT("docking");
        switch (Phase)
        {
        case EDTTTruckPhase::ToDepot: return TEXT("to a depot");
        case EDTTTruckPhase::Parked: return TEXT("parked");
        case EDTTTruckPhase::Assigned: return TEXT("on a lap");
        case EDTTTruckPhase::Manual: return TEXT("manual");
        default: return TEXT("dispatch");
        }
    }

    FString ReasonText(EDTTSkipReason Reason)
    {
        switch (Reason)
        {
        case EDTTSkipReason::ItemClassNotFound: return TEXT("the resource in the tag is no item");
        case EDTTSkipReason::Form: return TEXT("solid against fluid");
        case EDTTSkipReason::ForeignCargo: return TEXT("the truck is carrying something else");
        case EDTTSkipReason::NoCargo: return TEXT("nothing on board to deliver");
        case EDTTSkipReason::Unreachable: return TEXT("no road to it");
        case EDTTSkipReason::NoProducerWithStock: return TEXT("no producer with stock");
        case EDTTSkipReason::NoSuitableTruck: return TEXT("no free truck that fits");
        default: return TEXT("-");
        }
    }
}

void UDynamicTimeTableWidget::BuildTimetableHeader(UHorizontalBox* Row)
{
    if (!Row) return;
    HeadCell(WidgetTree, Row, FString(), DTTColIcon);
    HeadCell(WidgetTree, Row, TEXT("TRUCK / STATION"), DTTColTruck);
    HeadCell(WidgetTree, Row, TEXT("STATE"), DTTColPhase);
    HeadCell(WidgetTree, Row, TEXT("LAP"), 0.0f);
    HeadCell(WidgetTree, Row, TEXT("LEFT"), DTTColLeft, HAlign_Right);
    HeadCell(WidgetTree, Row, TEXT("CARGO"), DTTColCargo, HAlign_Right);
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

    // Three blocks on one page, each behind a line of its own: what is driving, what is waiting,
    // and what the dispatcher could not do about it. Kind keeps them apart in the keys.
    struct FEntry { int32 Kind = 0; int32 Index = 0; FString Key; };
    TArray<FEntry> Entries;
    // Driving is driving: a truck held at a depot or docked still has metres left on paper.
    int32 Driving = 0;
    for (const FDTTLapRow& Lap : Laps)
    {
        if (Lap.DistanceLeftMetres > 0 && Lap.bAutopilot && !Lap.bDocking && Lap.Phase != EDTTTruckPhase::Parked) ++Driving;
    }

    Entries.Add(FEntry{ 3, 0, FString::Printf(TEXT("h|laps|%d"), Driving) });
    for (int32 Index = 0; Index < Laps.Num(); ++Index)
    {
        if (!Keep(Laps[Index].TruckName, Laps[Index].ResourceId)) continue;
        Entries.Add(FEntry{ 0, Index, FString::Printf(TEXT("l|%s|%d"), *DTTTruckKey(Laps[Index].TruckName), Laps[Index].RouteStops.Num()) });
    }
    Entries.Add(FEntry{ 4, 0, FString::Printf(TEXT("h|queue|%d"), Queue.Num()) });
    for (int32 Index = 0; Index < Queue.Num(); ++Index)
    {
        if (!Keep(Queue[Index].Name, Queue[Index].ResourceId)) continue;
        Entries.Add(FEntry{ 1, Index, FString::Printf(TEXT("q|%s"), *Queue[Index].DockingPathNodeGuid.ToString(EGuidFormats::Short)) });
    }
    Entries.Add(FEntry{ 5, 0, FString::Printf(TEXT("h|why|%d"), Reasons.Num()) });
    for (int32 Index = 0; Index < Reasons.Num(); ++Index)
    {
        if (!Keep(Reasons[Index].ResourceId, FString())) continue;
        Entries.Add(FEntry{ 2, Index, FString::Printf(TEXT("r|%s|%d"), *Reasons[Index].ResourceId, int32(Reasons[Index].Reason)) });
    }

    TArray<FString> Keys;
    for (const FEntry& Entry : Entries) Keys.Add(Entry.Key);

    if (SummaryText)
    {
        SummaryText->SetText(FText::FromString(FString::Printf(TEXT("%d lap(s), %d driving, %d open request(s) - %s"),
            Laps.Num(), Driving, Queue.Num(), IsRemoteClient() ? TEXT("server snapshot") : TEXT("live"))));
    }

    SyncCards(TimetableScrollBox, TimetableCards, Keys, [&](int32 Index, FDTTCardSink& Sink)
    {
        const FEntry& Entry = Entries[Index];
        switch (Entry.Kind)
        {
        case 0: PopulateLapCard(Laps[Entry.Index], Sink); break;
        case 1: PopulateQueueCard(Queue[Entry.Index], Sink); break;
        case 2: PopulateReasonCard(Reasons[Entry.Index], Sink); break;
        case 3: PopulateSectionCard(TEXT("Laps"), FString::Printf(TEXT("%d truck(s), %d driving"), Laps.Num(), Driving), Sink); break;
        case 4: PopulateSectionCard(TEXT("Queue"), Queue.Num() == 0 ? TEXT("nobody is asking") : FString::Printf(TEXT("%d open request(s), longest wait first"), Queue.Num()), Sink); break;
        default: PopulateSectionCard(TEXT("Why nothing moved"), Reasons.Num() == 0 ? TEXT("the last pass placed everything it could") : TEXT("from the dispatcher's last pass"), Sink); break;
        }
    });
}

void UDynamicTimeTableWidget::PopulateSectionCard(const FString& Title, const FString& Summary, FDTTCardSink& Sink)
{
    UVerticalBox* Body = Sink.Root(FLinearColor(0.05f, 0.08f, 0.10f, 1.0f), FMargin(10, 6));
    UHorizontalBox* Line = Sink.HBox(Body);
    Sink.Text(Line, Title, 16, DTTCyan, false, FMargin(0, 0, 10, 0));
    Sink.Text(Line, Summary, 12, DTTGrey, false, FMargin(0), true);
}

void UDynamicTimeTableWidget::PopulateLapCard(const FDTTLapRow& Row, FDTTCardSink& Sink)
{
    const bool bMoving = Row.DistanceLeftMetres > 0 && Row.bAutopilot && !Row.bDocking && Row.Phase != EDTTTruckPhase::Parked;
    UVerticalBox* Body = Sink.Root(bMoving ? DTTCardTintOpen : DTTCardTint, FMargin(0));

    FDTTRowTarget None;
    UHorizontalBox* Line = Sink.Row(Body, DTTRowHeight, None, FLinearColor::White);
    Sink.Icon(Line, IconFor(Row.ItemClass), DTTColIcon, FMargin(0, 0, 6, 0), true);
    Sink.Cell(Line, DTTTruckTitle(Row.TruckName), 14, Row.bAutopilot ? FLinearColor::White : DTTRed, DTTColTruck);
    Sink.Cell(Line, Row.bAutopilot ? PhaseText(Row.Phase, Row.bDocking) : TEXT("autopilot off"),
        12, Row.bAutopilot ? DTTGrey : DTTRed, DTTColPhase);

    // The lap itself: one mark per stop, the one the truck is driving to lit. A route is a
    // handful of stops, so this says where in the lap a truck is at a glance, which a list of
    // station names never did.
    Sink.Strip(Line, Row.RouteStops.Num(), Row.TargetIndex, bMoving ? DTTGreen : DTTSteel);
    Sink.Cell(Line, DistanceText(Row.DistanceLeftMetres), 12, Row.DistanceLeftMetres > 0 ? FLinearColor::White : DTTDim, DTTColLeft, HAlign_Right);
    Sink.Cell(Line, Row.CargoCapacityStacks > 0
        ? FString::Printf(TEXT("%d / %d st"), FMath::Max(0, Row.CargoStacks), Row.CargoCapacityStacks)
        : FString(TEXT("-")), 12, Row.CargoStacks > 0 ? FLinearColor::White : DTTDim, DTTColCargo, HAlign_Right);
}

void UDynamicTimeTableWidget::PopulateQueueCard(const FDTTQueueRow& Row, FDTTCardSink& Sink)
{
    const int32 OverdueMinutes = ViewSettings().OverdueRequestMinutes;
    const bool bOverdue = OverdueMinutes > 0 && Row.ClaimOwner.IsEmpty() && Row.WaitingSeconds >= OverdueMinutes * 60;
    const FLinearColor Tint = bOverdue ? DTTCardTintWarn : (Row.ClaimOwner.IsEmpty() ? DTTCardTintWait : DTTCardTintOpen);
    UVerticalBox* Body = Sink.Root(Tint, FMargin(0));

    FDTTRowTarget None;
    UHorizontalBox* Line = Sink.Row(Body, DTTRowHeight, None, FLinearColor::White);
    Sink.Icon(Line, IconFor(Row.ItemClass), DTTColIcon, FMargin(0, 0, 6, 0), true);
    Sink.Cell(Line, DTTStationName(Row.Name), 14, FLinearColor::White, DTTColTruck);
    Sink.Cell(Line, Row.ResourceId, 12, DTTCyan, DTTColPhase);
    Sink.Cell(Line, Row.ClaimOwner.IsEmpty()
        ? FString::Printf(TEXT("waiting  -  %d of %d stacks, calls below %d"),
            FMath::Max(0, Row.StockStacks), FMath::Max(0, Row.CapacityStacks), FMath::Max(0, Row.TriggerStacks))
        : FString::Printf(TEXT("%s is on the way"), *DTTTruckTitle(Row.ClaimOwner)),
        12, Row.ClaimOwner.IsEmpty() ? (bOverdue ? DTTRed : DTTYellow) : DTTGreen, 0.0f);
    Sink.Cell(Line, LapWaitText(Row.WaitingSeconds), 12, bOverdue ? DTTRed : DTTGrey, DTTColLeft, HAlign_Right);
    Sink.Cell(Line, FString(), 12, DTTDim, DTTColCargo, HAlign_Right);
}

void UDynamicTimeTableWidget::PopulateReasonCard(const FDTTReasonRow& Row, FDTTCardSink& Sink)
{
    UVerticalBox* Body = Sink.Root(DTTCardTint, FMargin(0));
    FDTTRowTarget None;
    UHorizontalBox* Line = Sink.Row(Body, DTTRowHeight, None, FLinearColor::White);
    Sink.Icon(Line, nullptr, DTTColIcon, FMargin(0, 0, 6, 0), true);
    Sink.Cell(Line, Row.ResourceId, 14, DTTCyan, DTTColTruck);
    Sink.Cell(Line, Row.OpenRequests > 0 ? FString::Printf(TEXT("%d open"), Row.OpenRequests) : FString(), 12, DTTYellow, DTTColPhase);
    Sink.Cell(Line, ReasonText(Row.Reason), 13, DTTGrey, 0.0f);
    Sink.Cell(Line, Row.Count > 1 ? FString::Printf(TEXT("x%d"), Row.Count) : FString(), 12, DTTDim, DTTColLeft, HAlign_Right);
    Sink.Cell(Line, FString(), 12, DTTDim, DTTColCargo, HAlign_Right);
}
