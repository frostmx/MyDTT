#include "DTTCardSink.h"

#include "DynamicTimeTableSubsystem.h"
#include "DynamicTimeTableRemoteCallObject.h"
#include "FGDroneStationInfo.h"
#include "Resources/FGItemDescriptor.h"
#include "Components/EditableTextBox.h"
#include "Components/ScrollBox.h"
#include "Algo/StableSort.h"

using namespace DTTUI;

// The Drones tab: one row per vanilla drone port. A port sends what its belts put into it and
// receives what its drone brings; the row shows both, the drone's hold, and where the drone is.
// Nothing on this page acts on a drone.

namespace
{
    /** One of the three fill bars of a row: sends, receives, drone. */
    constexpr float DTTColDroneBar = 84.0f;
    /** Two lines of a name: a port and its pair are both auto-named, and both may wrap. */
    constexpr float DTTDroneRowHeight = 56.0f;
    /** The distance to the pair: "1.7 km" at the widest. */
    constexpr float DTTColDroneDistance = 64.0f;

    template <typename T>
    int32 DTPOrder(const T& A, const T& B) { return A < B ? -1 : (B < A ? 1 : 0); }

    FString DTPDistance(int32 Meters)
    {
        if (Meters >= 1000) return FString::Printf(TEXT("%.1f km"), Meters / 1000.0f);
        return Meters > 0 ? FString::Printf(TEXT("%d m"), Meters) : FString(TEXT("-"));
    }

    FString DTPSlots(int32 Used, int32 Capacity)
    {
        return Capacity > 0 ? FString::Printf(TEXT("%d / %d"), Used, Capacity) : FString(TEXT("-"));
    }

    /** The bar between the two sides of a port's name becomes a line break. */
    FString DTPTwoLines(const FString& Name)
    {
        return Name.Replace(TEXT(" | "), TEXT("\n"));
    }

    bool DTPFlying(const FDTTDroneRow& Row)
    {
        switch (EDroneStatus(Row.Status))
        {
        case EDroneStatus::EDS_TAKEOFF:
        case EDroneStatus::EDS_EN_ROUTE:
        case EDroneStatus::EDS_DOCKING:
            return true;
        default:
            // Standing at the pair is the far end of a trip: the drone is out, doing its work.
            return Row.Place == EDTTDronePlace::AtPair || Row.Place == EDTTDronePlace::ToPair || Row.Place == EDTTDronePlace::ToHome;
        }
    }
}

void UDynamicTimeTableWidget::BuildDroneHeader(UHorizontalBox* Row)
{
    if (!Row) return;
    // One caption per cell of the row, with the same widths and margins; every caption sorts,
    // as on the Stations page.
    DroneSortLabels.SetNum(int32(EDTTDroneSort::Count));
    DroneSortCaptions.SetNum(int32(EDTTDroneSort::Count));
    auto Sortable = [this, Row](EDTTDroneSort Column, const TCHAR* Caption, float Width,
        EHorizontalAlignment Align = HAlign_Left, const FMargin& Margin = FMargin(0, 0, 6, 0))
    {
        FDTTRowTarget Target;
        Target.Action = EDTTRowAction::DroneSort;
        Target.Value = int32(Column);
        DroneSortLabels[int32(Column)] = HeadButton(WidgetTree, Row, Caption, Width, Align, Margin, RowActionDelegate, Target);
        DroneSortCaptions[int32(Column)] = Caption;
    };
    HeadCell(WidgetTree, Row, FString(), DTTColRowTile, HAlign_Left, FMargin(0, 0, 8, 0));
    // Both names are ports, and an auto-name of a two-sided port runs to the full 96 characters:
    // the two share the free width and each may run onto a second line.
    Sortable(EDTTDroneSort::Port, TEXT("PORT"), 0.0f);
    Sortable(EDTTDroneSort::Drone, TEXT("DRONE"), DTTColPhase);
    Sortable(EDTTDroneSort::Destination, TEXT("DESTINATION"), 0.0f);
    Sortable(EDTTDroneSort::Distance, TEXT("DIST"), DTTColDroneDistance, HAlign_Right);
    Sortable(EDTTDroneSort::Sends, TEXT("SENDS"), DTTColDroneBar, HAlign_Right);
    Sortable(EDTTDroneSort::Receives, TEXT("RECEIVES"), DTTColDroneBar, HAlign_Right);
    Sortable(EDTTDroneSort::Hold, TEXT("HOLD"), DTTColDroneBar, HAlign_Right);
    HeadCell(WidgetTree, Row, FString(), DTTColChevron);
}

void UDynamicTimeTableWidget::PopulateDroneCard(const FDTTDroneRow& Row, const FString& CardId, bool bExpanded, FDTTCardSink& Sink)
{
    const EDroneStatus Status = EDroneStatus(Row.Status);
    const bool bDrone = (Row.Flags & EDTTDroneFlags::HasDrone) != 0;
    const bool bNoFuel = bDrone && Status == EDroneStatus::EDS_NOT_ENOUGH_FUEL;
    const bool bStuck = bDrone && Status == EDroneStatus::EDS_CANNOT_UNLOAD;
    const bool bBlocks = bStuck && (Row.Flags & EDTTDroneFlags::UnloadBlocksExport);
    const bool bIdle = Row.ExportSlots == 0 && Row.ImportSlots == 0;

    // Red: the drone is grounded. Yellow: it cannot unload at a port that also has cargo waiting
    // to leave, so it holds that cargo up. Green: it is out on its trip, or it waits at a port that
    // only receives - a full buffer, which is what a buffer is for. Yellow again: the port holds
    // nothing either way. The order is the player's.
    FLinearColor Tint = DTTCardTint;
    if (bNoFuel) Tint = DTTCardTintWarn;
    else if (bBlocks) Tint = DTTCardTintWait;
    else if (bDrone && (DTPFlying(Row) || bStuck)) Tint = DTTCardTintOpen;
    else if (bIdle) Tint = DTTCardTintWait;
    UVerticalBox* Body = Sink.Root(bExpanded ? Lighten(Tint) : Tint, FMargin(0));

    FDTTRowTarget Toggle;
    Toggle.Action = EDTTRowAction::ToggleExpandDrone;
    Toggle.CardKey = CardId;
    UHorizontalBox* Line = Sink.Row(Body, DTTDroneRowHeight, Toggle, FLinearColor::White);

    // Two tiles, one over the other, in the order of the bars: what the port sends on top, what it
    // receives below. Each keeps its place with nothing on that side, so the pair reads the same
    // way in every row.
    UVerticalBox* Tiles = Sink.VBox(Line);
    if (Tiles) if (UHorizontalBoxSlot* S = Cast<UHorizontalBoxSlot>(Tiles->Slot)) S->SetVerticalAlignment(VAlign_Center);
    Sink.Tile(Tiles, IconFor(Row.SendsItem), TileColorFor(Row.SendsItem), DTTColRowTile, FMargin(0, 0, 8, 2));
    Sink.Tile(Tiles, IconFor(Row.ReceivesItem), TileColorFor(Row.ReceivesItem), DTTColRowTile, FMargin(0, 0, 8, 0));

    // A two-sided auto-name is two blocks around a bar, sending then receiving - the order of
    // the tiles and of the bars - and the row has two lines: one each.
    Sink.Cell(Line, DTPTwoLines(Row.Name), 14, DTTText, 0.0f, HAlign_Left, FMargin(0, 0, 6, 0), true);

    EDTTPill Pill = EDTTPill::Dispatch;
    FString Label = UDynamicTimeTableSubsystem::DroneStatusLabel(Row.Status);
    switch (Status)
    {
    case EDroneStatus::EDS_DOCKED:
    case EDroneStatus::EDS_LOADING:
    case EDroneStatus::EDS_UNLOADING:
        Pill = EDTTPill::Docked; break;
    case EDroneStatus::EDS_TAKEOFF:
    case EDroneStatus::EDS_EN_ROUTE:
    case EDroneStatus::EDS_DOCKING:
        Pill = EDTTPill::PoolAssigned; break;
    case EDroneStatus::EDS_CANNOT_UNLOAD:
        Pill = bBlocks ? EDTTPill::Manual : EDTTPill::PoolParked; break;
    case EDroneStatus::EDS_NOT_ENOUGH_FUEL:
        Pill = EDTTPill::AutopilotOff; break;
    default: break;
    }
    if (!bDrone) { Pill = EDTTPill::Dispatch; Label = TEXT("No drone"); }
    Sink.Pill(Line, Label, Pill, DTTColPhase);

    // The pair the drone flies to, named plainly: where the drone is right now is the pill's
    // business. A port without a drone is a destination itself, and the count of ports flying to
    // it is the only route it has.
    FString Destination;
    FLinearColor DestinationColour = DTTGrey;
    if (!Row.PairedName.IsEmpty())
    {
        Destination = DTPTwoLines(Row.PairedName);
        if (Row.Place == EDTTDronePlace::AtPair || Row.Place == EDTTDronePlace::ToHome) DestinationColour = DTTSteel;
    }
    else if (Row.InLinks > 0) Destination = FString::Printf(TEXT("< %d port%s here"), Row.InLinks, Row.InLinks == 1 ? TEXT(" flies") : TEXT("s fly"));
    else { Destination = TEXT("no destination"); DestinationColour = bDrone ? DTTYellow : DTTDim; }
    if (Row.Flags & EDTTDroneFlags::Queue) Destination += TEXT("  (queue)");
    Sink.Cell(Line, Destination, 13, DestinationColour, 0.0f, HAlign_Left, FMargin(0, 0, 6, 0), true);
    Sink.Cell(Line, DTPDistance(Row.DistanceMeters), 13, Row.DistanceMeters > 0 ? DTTGrey : DTTDim, DTTColDroneDistance, HAlign_Right);

    Sink.CellBar(Line, DTPSlots(Row.ExportSlots, Row.ExportCapacity), Row.ExportSlots > 0 ? DTTText : DTTDim,
        Row.ExportFill / 100.0f, Row.ExportSlots > 0 ? DTTCyan : DTTDim, DTTColDroneBar);
    Sink.CellBar(Line, DTPSlots(Row.ImportSlots, Row.ImportCapacity), Row.ImportSlots > 0 ? DTTText : DTTDim,
        Row.ImportFill / 100.0f, Row.ImportSlots > 0 ? DTTGreen : DTTDim, DTTColDroneBar);
    Sink.CellBar(Line, bDrone ? DTPSlots(Row.CargoSlots, Row.CargoCapacity) : FString(TEXT("-")), Row.CargoSlots > 0 ? DTTText : DTTDim,
        bDrone ? Row.CargoFill / 100.0f : 0.0f, Row.CargoSlots > 0 ? DTTSteel : DTTDim, DTTColDroneBar);
    Sink.Cell(Line, bExpanded ? TEXT("v") : TEXT(">"), 13, DTTDim, DTTColChevron, HAlign_Center);

    // The flag is part of the card's key, so a build pass and an update pass for one key always
    // walk the same shape - which is the only reason this may be conditional at all.
    if (!bExpanded) return;

    UVerticalBox* Detail = Sink.VBox(Body, FMargin(10, 2, 10, 8));

    // Three inventories, three lines each, always: a line that has no item stays blank rather than
    // disappearing, or the card's shape would depend on what happens to lie in the port.
    auto Inventory = [&](const TCHAR* Caption, const TArray<FDTTItemAmount>& Items, int32 Slots, int32 Capacity, bool bKnown)
    {
        Sink.Text(Detail, FString::Printf(TEXT("%s  %s"), Caption, bKnown ? *DTPSlots(Slots, Capacity) : TEXT("-")), 12, DTTDim, false, FMargin(0, 6, 0, 0));
        for (int32 Index = 0; Index < 3; ++Index)
        {
            const bool bLine = Items.IsValidIndex(Index);
            UHorizontalBox* ItemRow = Sink.HBox(Detail, FMargin(0, 1, 0, 0));
            Sink.Icon(ItemRow, bLine ? IconFor(Items[Index].ItemClass) : nullptr, 18, FMargin(0, 0, 6, 0), true);
            FString ItemName;
            if (bLine)
            {
                UClass* ItemClass = Items[Index].ItemClass.Get();
                ItemName = FString::Printf(TEXT("%s  %d"), ItemClass ? *UFGItemDescriptor::GetItemName(ItemClass).ToString() : TEXT("?"), Items[Index].Count);
            }
            Sink.Text(ItemRow, ItemName, 13, DTTText);
        }
    };
    Inventory(TEXT("Sends (belts bring, the drone takes)"), Row.ExportItems, Row.ExportSlots, Row.ExportCapacity, true);
    Inventory(TEXT("Receives (the drone brings, belts take)"), Row.ImportItems, Row.ImportSlots, Row.ImportCapacity, true);
    Inventory(TEXT("Drone hold"), Row.CargoItems, Row.CargoSlots, Row.CargoCapacity, bDrone);

    FString FuelLine;
    if (UClass* FuelClass = Row.FuelClass.Get())
    {
        FuelLine = FString::Printf(TEXT("Fuel: %s %d"), *UFGItemDescriptor::GetItemName(FuelClass).ToString(), Row.FuelCount);
        if (Row.FuelPerTrip > 0.0f)
        {
            FuelLine += FString::Printf(TEXT(", %.1f a round trip"), Row.FuelPerTrip);
            if (Row.FuelCount > 0) FuelLine += FString::Printf(TEXT(", enough for %d"), int32(Row.FuelCount / Row.FuelPerTrip));
        }
    }
    else FuelLine = TEXT("Fuel: none");
    Sink.Text(Detail, FuelLine, 12, bNoFuel ? DTTRed : DTTGrey, true, FMargin(0, 8, 0, 0));

    FString TripLine;
    if (bDrone && Row.RoundTripSeconds > 0)
    {
        TripLine = FString::Printf(TEXT("Round trip %d s; the last one brought %d and took %d"), Row.RoundTripSeconds, Row.TripItemsIn, Row.TripItemsOut);
    }
    else if (bDrone) TripLine = TEXT("No round trip recorded yet");
    else TripLine = Row.InLinks > 0
        ? FString::Printf(TEXT("%d port%s send their drones here"), Row.InLinks, Row.InLinks == 1 ? TEXT("") : TEXT("s"))
        : FString(TEXT("Nobody flies here"));
    Sink.Text(Detail, TripLine, 12, DTTGrey, true, FMargin(0, 2, 0, 0));

    FString Note;
    if (bBlocks) Note = TEXT("The drone cannot unload, and cargo waits to leave behind it: make room on the receiving belt.");
    else if (bStuck) Note = TEXT("The drone waits with a full hold until the belts take what is here. Nothing else is waiting on it.");
    else if (bNoFuel) Note = TEXT("The port has no fuel for the round trip.");
    else if (bDrone && Row.PairedName.IsEmpty()) Note = TEXT("No destination: the drone stays docked and keeps loading.");
    Sink.Text(Detail, Note, 12, bNoFuel || bBlocks ? DTTYellow : DTTGrey, true, FMargin(0, 4, 0, 0));
}

void UDynamicTimeTableWidget::RefreshDroneDisplay()
{
    TArray<FDTTDroneRow> Rows;
    if (IsRemoteClient())
    {
        if (RemoteCallObject) Rows = RemoteCallObject->GetSnapshot().DroneRows;
    }
    else if (UDynamicTimeTableSubsystem* Subsystem = GetWorld()->GetSubsystem<UDynamicTimeTableSubsystem>())
    {
        Subsystem->BuildDroneRows(Rows);
    }

    // The id is the info actor, which outlives any rename; the key adds only whether the card is
    // open, because the card draws the same widgets whatever the port holds.
    const FString Filter = DroneFilter ? DroneFilter->GetText().ToString().TrimStartAndEnd() : FString();
    TArray<const FDTTDroneRow*> Shown;
    TArray<FString> Ids;
    TArray<FString> Keys;
    TArray<bool> Expanded;
    int32 Drones = 0, Flying = 0, Stuck = 0, Blocking = 0, NoFuel = 0;
    for (const FDTTDroneRow& Row : Rows)
    {
        if (Row.Flags & EDTTDroneFlags::HasDrone)
        {
            ++Drones;
            const EDroneStatus Status = EDroneStatus(Row.Status);
            if (Status == EDroneStatus::EDS_NOT_ENOUGH_FUEL) ++NoFuel;
            else if (Status == EDroneStatus::EDS_CANNOT_UNLOAD) { ++Stuck; if (Row.Flags & EDTTDroneFlags::UnloadBlocksExport) ++Blocking; }
            else if (DTPFlying(Row)) ++Flying;
        }
        if (!UDynamicTimeTableSubsystem::DroneRowMatches(Row, Filter)) continue;
        Shown.Add(&Row);
    }

    if (DroneSort != EDTTDroneSort::None)
    {
        // The drone column orders by what needs a look: no fuel, then holding cargo up, then the
        // rest by status, ports without a drone last. A bar orders by how full it is.
        auto DroneRank = [](const FDTTDroneRow& R)
        {
            if (!(R.Flags & EDTTDroneFlags::HasDrone)) return 100;
            const EDroneStatus Status = EDroneStatus(R.Status);
            if (Status == EDroneStatus::EDS_NOT_ENOUGH_FUEL) return 0;
            if (Status == EDroneStatus::EDS_CANNOT_UNLOAD) return (R.Flags & EDTTDroneFlags::UnloadBlocksExport) ? 1 : 2;
            return 10 + int32(Status);
        };
        auto Fill = [](uint8 Slots, uint8 Capacity, uint8 FillPercent) { return Capacity > 0 ? int32(FillPercent) * 1000 + Slots : -1; };
        auto Compare = [&](const FDTTDroneRow& A, const FDTTDroneRow& B) -> int32
        {
            switch (DroneSort)
            {
            case EDTTDroneSort::Port: return A.Name.Compare(B.Name, ESearchCase::IgnoreCase);
            case EDTTDroneSort::Drone: return DTPOrder(DroneRank(A), DroneRank(B));
            case EDTTDroneSort::Destination: return A.PairedName.Compare(B.PairedName, ESearchCase::IgnoreCase);
            case EDTTDroneSort::Distance: return DTPOrder(A.DistanceMeters, B.DistanceMeters);
            case EDTTDroneSort::Sends: return DTPOrder(Fill(A.ExportSlots, A.ExportCapacity, A.ExportFill), Fill(B.ExportSlots, B.ExportCapacity, B.ExportFill));
            case EDTTDroneSort::Receives: return DTPOrder(Fill(A.ImportSlots, A.ImportCapacity, A.ImportFill), Fill(B.ImportSlots, B.ImportCapacity, B.ImportFill));
            case EDTTDroneSort::Hold: return DTPOrder(Fill(A.CargoSlots, A.CargoCapacity, A.CargoFill), Fill(B.CargoSlots, B.CargoCapacity, B.CargoFill));
            default: return 0;
            }
        };
        Algo::StableSort(Shown, [&](const FDTTDroneRow* A, const FDTTDroneRow* B)
        {
            int32 Result = Compare(*A, *B);
            if (bDroneSortDescending) Result = -Result;
            if (Result != 0) return Result < 0;
            return A->Name.Compare(B->Name, ESearchCase::IgnoreCase) < 0;
        });
    }
    for (int32 Column = 0; Column < DroneSortLabels.Num(); ++Column)
    {
        UTextBlock* Label = DroneSortLabels[Column].Get();
        if (!Label || !DroneSortCaptions.IsValidIndex(Column)) continue;
        const bool bOn = Column == int32(DroneSort);
        Label->SetText(FText::FromString(bOn
            ? DroneSortCaptions[Column] + TEXT(" ") + FString::Chr(bDroneSortDescending ? 0x25BC : 0x25B2)
            : DroneSortCaptions[Column]));
        Label->SetColorAndOpacity(FSlateColor(bOn ? DTTCyan : FLinearColor(0.72f, 0.78f, 0.82f)));
    }

    for (const FDTTDroneRow* RowPtr : Shown)
    {
        const FDTTDroneRow& Row = *RowPtr;
        const FString Id = Row.Port ? GetNameSafe(Row.Port) : FString::Printf(TEXT("name:%s"), *Row.Name);
        const bool bOpen = ExpandedDroneKey == Id;
        Ids.Add(Id);
        Expanded.Add(bOpen);
        Keys.Add(FString::Printf(TEXT("%s|%s"), *Id, bOpen ? TEXT("E") : TEXT("c")));
    }
    if (Shown.Num() == 0) Keys.Add(Rows.Num() == 0 ? TEXT("empty") : TEXT("nomatch"));

    FDTTStatusLine Line;
    Line.Count(Rows.Num(), TEXT("ports")).Count(Drones, TEXT("drones")).Count(Flying, TEXT("flying"), DTTCyan)
        .Count(Stuck, TEXT("cannot unload"));
    if (Blocking > 0) Line.Count(Blocking, TEXT("hold up cargo"), DTTYellow);
    if (NoFuel > 0) Line.Count(NoFuel, TEXT("no fuel"), DTTRed);
    SetStatusLine(Line, FString());

    SyncCards(DroneScrollBox, DroneCards, Keys, [&](int32 Index, FDTTCardSink& Sink)
    {
        if (Shown.IsValidIndex(Index)) { PopulateDroneCard(*Shown[Index], Ids[Index], Expanded[Index], Sink); return; }
        UVerticalBox* Body = Sink.Root(DTTCardTint);
        const bool bNone = Rows.Num() == 0;
        DTTCardHeader(Sink, Body, bNone ? TEXT("No drone ports") : TEXT("Nothing matches the filter"), DTTGrey, FString());
        Sink.Text(Body, bNone ? TEXT("Every vanilla drone port shows up here with what it sends, what it receives and where its drone is.") : TEXT(""),
            13, DTTGrey, true, FMargin(0, 4, 0, 0));
    });
}
