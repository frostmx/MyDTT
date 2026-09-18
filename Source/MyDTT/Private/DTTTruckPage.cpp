#include "DTTCardSink.h"

#include "DynamicTimeTableSubsystem.h"
#include "DynamicTimeTableRemoteCallObject.h"
#include "Resources/FGItemDescriptor.h"
#include "WheeledVehicles/FGWheeledVehicleIdentifier.h"
#include "Components/EditableTextBox.h"
#include "Components/ScrollBox.h"

using namespace DTTUI;

void UDynamicTimeTableWidget::BuildTruckHeader(UHorizontalBox* Row)
{
    if (!Row) return;
    HeadCell(WidgetTree, Row, FString(), DTTColIcon);
    HeadCell(WidgetTree, Row, TEXT("TRUCK"), DTTColTruck);
    HeadCell(WidgetTree, Row, TEXT("PHASE"), DTTColPhase);
    HeadCell(WidgetTree, Row, TEXT("NEXT STOP"), 0.0f);
    HeadCell(WidgetTree, Row, TEXT("CARGO"), DTTColCargo, HAlign_Right);
    HeadCell(WidgetTree, Row, FString(), DTTColChevron);
}

void UDynamicTimeTableWidget::PopulateTruckCard(const FDTTTruckRow& Row, const FString& CardId, bool bExpanded, FDTTCardSink& Sink)
{
    FLinearColor PhaseColor = DTTGrey;
    FString PhaseLabel = TEXT("Dispatch");
    if (Row.bPool)
    {
        PhaseLabel = TEXT("Pool - ") + UDynamicTimeTableSubsystem::PhaseLabel(Row.Phase);
        switch (Row.Phase)
        {
        case EDTTTruckPhase::Parked: PhaseColor = DTTSteel; break;
        case EDTTTruckPhase::Assigned: PhaseColor = DTTCyan; break;
        case EDTTTruckPhase::ToDepot: PhaseColor = DTTSteel; break;
        case EDTTTruckPhase::Manual: PhaseColor = DTTOrange; break;
        default: break;
        }
    }
    if (!Row.bAutopilot) { PhaseLabel = TEXT("Manual"); PhaseColor = DTTOrange; }
    const bool bWarn = !Row.bAutopilot || (Row.bPool && Row.Phase == EDTTTruckPhase::Manual);
    const FLinearColor Tint = bWarn ? DTTCardTintWarn : DTTCardTint;
    UVerticalBox* Body = Sink.Root(bExpanded ? Lighten(Tint) : Tint, FMargin(0));

    FDTTRowTarget Toggle;
    Toggle.Action = EDTTRowAction::ToggleExpandTruck;
    Toggle.CardKey = CardId;
    UHorizontalBox* Line = Sink.Row(Body, DTTRowHeight, Toggle, FLinearColor::White);

    // What is on board when it is one item; the truck's own resource when only the count is known.
    UTexture2D* CargoIcon = IconFor(Row.CargoItemClass);
    if (!CargoIcon && Row.CargoStacks > 0) CargoIcon = IconFor(Row.ItemClass);
    if (!CargoIcon) CargoIcon = IconFor(Row.ItemClass);
    Sink.Icon(Line, CargoIcon, DTTColIcon, FMargin(0, 0, 6, 0));

    // The mod names a dispatch truck "<resource> -> <dock>", so the resource leads and the name is
    // only worth showing when the player chose one.
    const FString TruckTitle = DTTTruckTitle(Row.Name);
    const bool bResource = !Row.ResourceId.IsEmpty();
    const FString Tail = bResource ? DTTTruckTail(TruckTitle, Row.ResourceId) : FString();
    FString Title = bResource ? Row.ResourceId : TruckTitle;
    if (!Tail.IsEmpty()) Title += TEXT("  ") + Tail;
    Sink.Cell(Line, Title, 14, bResource ? DTTCyan : FLinearColor::White, DTTColTruck);
    Sink.Cell(Line, PhaseLabel, 12, PhaseColor, DTTColPhase);

    FString NextStop;
    if (Row.bDocking) NextStop = Row.RouteStops.IsValidIndex(Row.TargetIndex) ? FString::Printf(TEXT("docked at %s"), *Row.RouteStops[Row.TargetIndex]) : FString(TEXT("docked"));
    else if (Row.RouteStops.IsValidIndex(Row.TargetIndex)) NextStop = FString(TEXT("> ")) + Row.RouteStops[Row.TargetIndex];
    else if (Row.RouteStops.Num() == 0) NextStop = TEXT("no route");
    else NextStop = TEXT("no target");
    Sink.Cell(Line, NextStop, 13, Row.bDocking ? DTTSteel : FLinearColor(0.82f, 0.87f, 0.9f), 0.0f);

    const FString Cargo = Row.CargoCapacityStacks > 0
        ? FString::Printf(TEXT("%d / %d st"), FMath::Max(0, Row.CargoStacks), Row.CargoCapacityStacks)
        : FString(TEXT("-"));
    Sink.Cell(Line, Cargo, 12, Row.CargoStacks > 0 ? FLinearColor::White : DTTDim, DTTColCargo, HAlign_Right);
    Sink.Cell(Line, bExpanded ? TEXT("v") : TEXT(">"), 13, DTTDim, DTTColChevron, HAlign_Center);

    // The flag is part of the card's key, so a build pass and an update pass for one key always
    // walk the same shape - which is the only reason this may be conditional at all.
    if (!bExpanded) return;

    UVerticalBox* Detail = Sink.VBox(Body, FMargin(10, 2, 10, 8));

    // Where it is going: the stops in order, the current target lit, runs of one stop as one chip.
    UHorizontalBox* RouteRow = Sink.HBox(Detail, FMargin(0, 2, 0, 0));
    Sink.Text(RouteRow, TEXT("Route"), 12, DTTDim, false, FMargin(0, 0, 8, 0));
    UWrapBox* Stops = Sink.Wrap(RouteRow);
    if (Row.RouteStops.Num() == 0) Sink.Text(Stops, TEXT("no route"), 13, DTTDim);
    int32 Chips = 0;
    for (int32 Index = 0; Index < Row.RouteStops.Num(); ++Index)
    {
        if (Index > 0 && Row.RouteStops[Index] == Row.RouteStops[Index - 1]) continue;
        int32 End = Index;
        while (End + 1 < Row.RouteStops.Num() && Row.RouteStops[End + 1] == Row.RouteStops[Index]) ++End;
        if (Chips++ > 0) Sink.Text(Stops, TEXT(">"), 13, DTTDim);
        const bool bTarget = Row.TargetIndex >= Index && Row.TargetIndex <= End;
        Sink.Text(Stops, Row.RouteStops[Index], 13, bTarget ? DTTCyan : FLinearColor(0.75f, 0.8f, 0.85f));
    }

    Sink.Text(Detail, Row.bPool ? FString::Printf(TEXT("depot %s"), Row.DepotName.IsEmpty() ? TEXT("-") : *Row.DepotName) : FString(), 12, DTTGrey, true, FMargin(0, 2, 0, 0));

    const FString CargoLine = Row.CargoCapacityStacks >= 0
        ? FString::Printf(TEXT("Cargo %d / %d stacks"), Row.CargoStacks, Row.CargoCapacityStacks)
        : FString(TEXT("Cargo unknown"));
    const float Percent = Row.CargoCapacityStacks > 0 ? float(FMath::Max(Row.CargoStacks, 0)) / float(Row.CargoCapacityStacks) : 0.0f;
    Sink.Text(Detail, CargoLine, 13, FLinearColor::White, false, FMargin(0, 6, 0, 0));
    Sink.Bar(Detail, Percent, Row.CargoStacks > 0 ? DTTCyan : DTTDim);
    Sink.Text(Detail, Row.CargoNote, 12, DTTGrey, true);
    Sink.Text(Detail, Row.PlannedSummary.IsEmpty() ? FString() : TEXT("Planned: ") + Row.PlannedSummary, 12, FLinearColor(0.7f, 0.8f, 0.85f), true, FMargin(0, 2, 0, 0));

    // The four operations, the same ones /dtt park and /dtt release run. A control is always here
    // and only its enabled state changes, so the shape of the card does not depend on what a truck
    // happens to be doing: a recycled button stays the button it was.
    AFGWheeledVehicleIdentifier* Truck = Row.Truck.Get();
    auto MakeTarget = [&](EDTTRowAction Action)
    {
        FDTTRowTarget Target;
        Target.Action = Action;
        Target.Truck = Truck;
        Target.CardKey = CardId;
        return Target;
    };
    const bool bHandle = IsValid(Truck);
    const bool bParked = Row.bPool && Row.Phase == EDTTTruckPhase::Parked;
    UHorizontalBox* Actions = Sink.HBox(Detail, FMargin(0, 8, 0, 0));
    Sink.Button(Actions, TEXT("Rebuild the lap"), MakeTarget(EDTTRowAction::TruckRebuildLap), true, bHandle && Row.bAutopilot && !Row.bDocking);
    Sink.Button(Actions, TEXT("Send to a depot"), MakeTarget(EDTTRowAction::TruckPark), true, bHandle && Row.bAutopilot && !bParked);
    Sink.Button(Actions, TEXT("Release"), MakeTarget(EDTTRowAction::TruckRelease), true, bHandle && bParked);
    if (Row.bAutopilot)
    {
        const bool bArmed = IsArmed(EDTTRowAction::TruckAutopilotOff, CardId);
        Sink.Button(Actions, bArmed ? TEXT("Confirm?") : TEXT("Autopilot off"), MakeTarget(EDTTRowAction::TruckAutopilotOff), true, bHandle);
    }
    else
    {
        Sink.Button(Actions, TEXT("Autopilot on"), MakeTarget(EDTTRowAction::TruckAutopilotOn), true, bHandle);
    }
    Sink.Text(Detail, bHandle ? FString() : TEXT("The server has not sent this truck's identity yet; the buttons come alive with the next snapshot."),
        11, DTTDim, true, FMargin(0, 4, 0, 0));
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

    // The id is the truck, not its display name (that changes with every target). The key adds
    // everything that decides the card's shape: whether it is open, whether its autopilot is on
    // (that swaps one button for another) and how many route chips the open card draws.
    const FString Filter = TruckFilter ? TruckFilter->GetText().ToString().TrimStartAndEnd() : FString();
    TArray<const FDTTTruckRow*> Shown;
    TArray<FString> Ids;
    TArray<FString> Keys;
    TArray<bool> Expanded;
    int32 Pool = 0, Parked = 0;
    for (const FDTTTruckRow& Row : Rows)
    {
        if (Row.bPool) { ++Pool; if (Row.Phase == EDTTTruckPhase::Parked) ++Parked; }
        if (!Filter.IsEmpty() && !Row.Name.Contains(Filter) && !Row.ResourceId.Contains(Filter)) continue;
        Shown.Add(&Row);
        const FString Id = DTTTruckKey(Row.Name);
        const bool bOpen = ExpandedTruckKey == Id;
        Ids.Add(Id);
        Expanded.Add(bOpen);
        Keys.Add(FString::Printf(TEXT("%s|%d|%d|%s|%d"), *Id, Row.bPool ? 1 : 0, Row.bAutopilot ? 1 : 0,
            bOpen ? TEXT("E") : TEXT("c"), bOpen ? DTTRouteChips(Row.RouteStops) : 0));
    }
    if (Shown.Num() == 0) Keys.Add(Rows.Num() == 0 ? TEXT("empty") : TEXT("nomatch"));
    if (SummaryText)
    {
        SummaryText->SetText(FText::FromString(FString::Printf(TEXT("%d truck(s), %d in the pool, %d parked - %s"),
            Rows.Num(), Pool, Parked, IsRemoteClient() ? TEXT("server snapshot") : TEXT("live"))));
    }

    SyncCards(TruckScrollBox, TruckCards, Keys, [&](int32 Index, FDTTCardSink& Sink)
    {
        if (Shown.IsValidIndex(Index)) { PopulateTruckCard(*Shown[Index], Ids[Index], Expanded[Index], Sink); return; }
        UVerticalBox* Body = Sink.Root(DTTCardTint);
        const bool bNoTrucks = Rows.Num() == 0;
        DTTCardHeader(Sink, Body, bNoTrucks ? TEXT("No managed trucks") : TEXT("Nothing matches the filter"), DTTGrey, FString());
        Sink.Text(Body, bNoTrucks ? TEXT("A truck whose first stop is a (Resource) LOAD station runs dispatch laps; one whose route contains a (D) depot joins the pool "
            "(or /dtt park <truck>).") : TEXT(""), 13, DTTGrey, true, FMargin(0, 4, 0, 0));
    });
}
