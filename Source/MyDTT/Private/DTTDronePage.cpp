#include "DTTCardSink.h"

#include "DynamicTimeTableSubsystem.h"
#include "DynamicTimeTableRemoteCallObject.h"
#include "FGDroneStationInfo.h"
#include "Resources/FGItemDescriptor.h"
#include "Components/EditableTextBox.h"
#include "Components/ScrollBox.h"

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

    FString DTPSlots(int32 Used, int32 Capacity)
    {
        return Capacity > 0 ? FString::Printf(TEXT("%d / %d"), Used, Capacity) : FString(TEXT("-"));
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
    HeadCell(WidgetTree, Row, FString(), DTTColRowTile, HAlign_Left, FMargin(0, 0, 8, 0));
    // Both names are ports, and an auto-name of a two-sided port runs to the full 96 characters:
    // the two share the free width and each may run onto a second line.
    HeadCell(WidgetTree, Row, TEXT("PORT"), 0.0f);
    HeadCell(WidgetTree, Row, TEXT("DRONE"), DTTColPhase);
    HeadCell(WidgetTree, Row, TEXT("DESTINATION"), 0.0f);
    HeadCell(WidgetTree, Row, TEXT("SENDS"), DTTColDroneBar, HAlign_Right);
    HeadCell(WidgetTree, Row, TEXT("RECEIVES"), DTTColDroneBar, HAlign_Right);
    HeadCell(WidgetTree, Row, TEXT("HOLD"), DTTColDroneBar, HAlign_Right);
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

    // The tile is what the port is for: what it receives first, else what it sends, else what its
    // drone happens to carry.
    TSoftClassPtr<UFGItemDescriptor> TileItem;
    if (Row.ImportItems.Num() > 0) TileItem = Row.ImportItems[0].ItemClass;
    else if (Row.ExportItems.Num() > 0) TileItem = Row.ExportItems[0].ItemClass;
    else if (Row.CargoItems.Num() > 0) TileItem = Row.CargoItems[0].ItemClass;
    Sink.Tile(Line, IconFor(TileItem), TileColorFor(TileItem), DTTColRowTile);

    Sink.Cell(Line, Row.Name, 14, DTTText, 0.0f, HAlign_Left, FMargin(0, 0, 6, 0), true);

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

    // Where the drone is against its pair. A port without a drone is a destination itself, and
    // the count of ports flying to it is the only route it has.
    FString Destination;
    FLinearColor DestinationColour = DTTGrey;
    if (!Row.PairedName.IsEmpty())
    {
        const TCHAR* Prefix = TEXT("> ");
        if (Row.Place == EDTTDronePlace::AtPair) Prefix = TEXT("at ");
        else if (Row.Place == EDTTDronePlace::ToHome) Prefix = TEXT("< ");
        Destination = FString::Printf(TEXT("%s%s"), Prefix, *Row.PairedName);
        // The cell wraps, and a wrap between the number and its unit would strand an "m" on a
        // line of its own: the space inside the distance is a non-breaking one.
        if (Row.DistanceMeters >= 1000) Destination += FString::Printf(TEXT("  %.1f km"), Row.DistanceMeters / 1000.0f);
        else if (Row.DistanceMeters > 0) Destination += FString::Printf(TEXT("  %d m"), Row.DistanceMeters);
        if (Row.Place == EDTTDronePlace::AtPair || Row.Place == EDTTDronePlace::ToHome) DestinationColour = DTTSteel;
    }
    else if (Row.InLinks > 0) Destination = FString::Printf(TEXT("< %d port%s here"), Row.InLinks, Row.InLinks == 1 ? TEXT(" flies") : TEXT("s fly"));
    else { Destination = TEXT("no destination"); DestinationColour = bDrone ? DTTYellow : DTTDim; }
    if (Row.Flags & EDTTDroneFlags::Queue) Destination += TEXT("  (queue)");
    Sink.Cell(Line, Destination, 13, DestinationColour, 0.0f, HAlign_Left, FMargin(0, 0, 6, 0), true);

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
