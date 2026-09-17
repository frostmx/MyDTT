#include "DTTCardSink.h"

#include "DynamicTimeTableSubsystem.h"
#include "DynamicTimeTableRemoteCallObject.h"
#include "Resources/FGItemDescriptor.h"
#include "Components/EditableTextBox.h"
#include "Components/ScrollBox.h"

using namespace DTTUI;

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
