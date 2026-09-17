#include "DTTCardSink.h"

#include "DynamicTimeTableSubsystem.h"
#include "DynamicTimeTableRemoteCallObject.h"
#include "Resources/FGItemDescriptor.h"
#include "Components/EditableTextBox.h"
#include "Components/ScrollBox.h"

using namespace DTTUI;

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
    // The cargo leads: the truck's own name is the mod's "<resource> -> <dock>" and would say it twice.
    const FString TruckTitle = DTTTruckTitle(Row.Name);
    const bool bResource = !Row.ResourceId.IsEmpty();
    Sink.Text(Head, bResource ? Row.ResourceId : TruckTitle, 17, bResource ? DTTCyan : FLinearColor::White, false, FMargin(0, 0, 10, 0), true);
    Sink.Text(Head, bResource ? DTTTruckTail(TruckTitle, Row.ResourceId) : FString(TEXT("no resource")), 14, bResource ? FLinearColor::White : DTTDim, false, FMargin(0, 0, 10, 0));
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
