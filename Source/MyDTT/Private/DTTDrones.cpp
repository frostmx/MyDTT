#include "DynamicTimeTableSubsystem.h"

#include "Buildables/FGBuildableDroneStation.h"
#include "FGDroneStationInfo.h"
#include "FGDroneSubsystem.h"
#include "FGDroneVehicle.h"
#include "FGFactoryConnectionComponent.h"
#include "FGInventoryComponent.h"
#include "Resources/FGItemDescriptor.h"

// The read-only half of the mod's drone support: what every vanilla drone port holds and where its
// drone is, for the Drones tab and /dtt drones. How the game runs a drone was taken apart from the
// server binary (see DYNAMICTIMETABLE.md, "4.18"); the two facts this file leans on are that belts
// fill a port's INPUT inventory and a drone empties it, and that a drone fills the OUTPUT inventory
// and the belts empty that - the comments on the two getters in the game's header say the opposite.

namespace
{
    /** How many different items of an inventory a row carries; the rest only count towards the fill. */
    constexpr int32 DTDMaxItemLines = 3;

    /** The connector that feeds the fuel slot carries this component tag; every other input is cargo. */
    const FName DTDFuelTag(TEXT("Fuel"));

    struct FDTDHold
    {
        TArray<FDTTItemAmount> Items;
        uint8 Slots = 0;
        uint8 Capacity = 0;
        uint8 Fill = 0;
    };

    FDTDHold DTDReadHold(UFGInventoryComponent* Inventory)
    {
        FDTDHold Out;
        if (!IsValid(Inventory)) return Out;

        const int32 Size = Inventory->GetSizeLinear();
        TMap<TSubclassOf<UFGItemDescriptor>, int32> Counts;
        int32 Slots = 0;
        // In stacks, so that a slot with one screw in it does not read as a full slot.
        double Stacks = 0.0;
        for (int32 Index = 0; Index < Size; ++Index)
        {
            FInventoryStack Stack;
            if (!Inventory->GetStackFromIndex(Index, Stack) || !Stack.HasItems()) continue;
            const TSubclassOf<UFGItemDescriptor> ItemClass = Stack.Item.GetItemClass();
            if (!ItemClass) continue;
            ++Slots;
            Counts.FindOrAdd(ItemClass) += Stack.NumItems;
            Stacks += double(Stack.NumItems) / double(FMath::Max(1, UFGItemDescriptor::GetStackSize(ItemClass)));
        }

        Counts.ValueSort([](int32 A, int32 B) { return A > B; });
        for (const TPair<TSubclassOf<UFGItemDescriptor>, int32>& Pair : Counts)
        {
            if (Out.Items.Num() >= DTDMaxItemLines) break;
            FDTTItemAmount& Line = Out.Items.AddDefaulted_GetRef();
            Line.ItemClass = Pair.Key.Get();
            Line.Count = Pair.Value;
        }
        Out.Slots = uint8(FMath::Clamp(Slots, 0, 255));
        Out.Capacity = uint8(FMath::Clamp(Size, 0, 255));
        Out.Fill = Size > 0 ? uint8(FMath::Clamp(FMath::RoundToInt(Stacks / double(Size) * 100.0), 0, 100)) : 0;
        // Something is there, however little: a bar that rounds to nothing would say "empty".
        if (Slots > 0 && Out.Fill == 0) Out.Fill = 1;
        return Out;
    }

    /** Whether a belt is connected on the given side, the fuel input left out. */
    bool DTDHasBelt(const AFGBuildableDroneStation* Station, EFactoryConnectionDirection Direction)
    {
        if (!IsValid(Station)) return false;
        for (const UFGFactoryConnectionComponent* Connection : Station->GetConnectionComponents())
        {
            if (!IsValid(Connection) || !Connection->IsConnected()) continue;
            if (Connection->GetDirection() != Direction) continue;
            if (Direction == EFactoryConnectionDirection::FCD_INPUT && Connection->ComponentHasTag(DTDFuelTag)) continue;
            return true;
        }
        return false;
    }

    bool DTDHasItems(UFGInventoryComponent* Inventory)
    {
        return IsValid(Inventory) && Inventory->GetFirstIndexWithItem() != INDEX_NONE;
    }

    FString DTDPortName(const AFGDroneStationInfo* Info)
    {
        return IsValid(Info) ? Info->GetBuildingTag_Implementation() : FString();
    }

    FString DTDItemList(const TArray<FDTTItemAmount>& Items)
    {
        TArray<FString> Parts;
        for (const FDTTItemAmount& Line : Items)
        {
            UClass* ItemClass = Line.ItemClass.Get();
            Parts.Add(FString::Printf(TEXT("%s %d"),
                ItemClass ? *UFGItemDescriptor::GetItemName(ItemClass).ToString() : TEXT("?"), Line.Count));
        }
        return Parts.Num() > 0 ? FString::Join(Parts, TEXT(", ")) : FString(TEXT("-"));
    }
}

void UDynamicTimeTableSubsystem::BuildDroneRows(TArray<FDTTDroneRow>& OutRows) const
{
    OutRows.Reset();
    AFGDroneSubsystem* Drones = AFGDroneSubsystem::Get(GetWorld());
    if (!IsValid(Drones)) return;

    for (AFGDroneStationInfo* Info : Drones->GetAllStations())
    {
        if (!IsValid(Info)) continue;
        // Server only: the building is not there on a client that stands far away, which is why the
        // rows are built here and sent rather than read off the client's own world.
        AFGBuildableDroneStation* Station = Info->GetStation();
        if (!IsValid(Station)) continue;

        FDTTDroneRow& Row = OutRows.AddDefaulted_GetRef();
        Row.Name = DTDPortName(Info);
        Row.Port = Info;
        Row.Status = uint8(Info->GetDroneStatus());
        Row.InLinks = Info->GetConnectedStations().Num();

        const AFGDroneStationInfo* Paired = Info->GetPairedStation();
        if (IsValid(Paired))
        {
            Row.PairedName = DTDPortName(Paired);
            Row.DistanceMeters = FMath::RoundToInt(FVector::Dist2D(Info->GetStationLocation(), Paired->GetStationLocation()) / 100.0);
        }

        const FDTDHold Export = DTDReadHold(Station->GetInputInventory());
        Row.ExportItems = Export.Items; Row.ExportSlots = Export.Slots; Row.ExportCapacity = Export.Capacity; Row.ExportFill = Export.Fill;
        const FDTDHold Import = DTDReadHold(Station->GetOutputInventory());
        Row.ImportItems = Import.Items; Row.ImportSlots = Import.Slots; Row.ImportCapacity = Import.Capacity; Row.ImportFill = Import.Fill;

        // The tile of a side survives the side being empty: what it holds now, else what it held
        // when last seen, else what the auto-name walk found on its belts.
        FDTTDroneSideItems& Seen = DroneSideItemsSeen.FindOrAdd(Info);
        if (Export.Items.Num() > 0) Seen.Sends = Export.Items[0].ItemClass;
        if (Import.Items.Num() > 0) Seen.Receives = Import.Items[0].ItemClass;
        Row.SendsItem = Seen.Sends;
        Row.ReceivesItem = Seen.Receives;
        if (Row.SendsItem.IsNull() || Row.ReceivesItem.IsNull())
        {
            if (const TArray<FDTTAutoNameReach>* Reaches = AutoNameDroneMachines.Find(Info))
            {
                for (const FDTTAutoNameReach& Reach : *Reaches)
                {
                    if (!Reach.ItemClass) continue;
                    TSoftClassPtr<UFGItemDescriptor>& Side = Reach.bRequester ? Row.ReceivesItem : Row.SendsItem;
                    if (Side.IsNull()) Side = TSoftClassPtr<UFGItemDescriptor>(Reach.ItemClass.Get());
                }
            }
        }

        if (DTDHasBelt(Station, EFactoryConnectionDirection::FCD_INPUT)) Row.Flags |= EDTTDroneFlags::ExportBelt;
        if (DTDHasBelt(Station, EFactoryConnectionDirection::FCD_OUTPUT)) Row.Flags |= EDTTDroneFlags::ImportBelt;
        if (Station->HasDronesInQueue()) Row.Flags |= EDTTDroneFlags::Queue;

        // The fuel the port holds; when it holds none, the fuel its drone last flew on, so the cost
        // of a trip can still be shown against an empty slot.
        const FDTDHold Fuel = DTDReadHold(Station->GetFuelInventory());
        TSubclassOf<UFGItemDescriptor> FuelClass = Fuel.Items.Num() > 0 ? TSubclassOf<UFGItemDescriptor>(Fuel.Items[0].ItemClass.Get()) : nullptr;
        if (FuelClass) Row.FuelCount = Fuel.Items[0].Count;
        else FuelClass = Info->GetDroneActiveFuelType();
        Row.FuelClass = FuelClass.Get();
        if (FuelClass)
        {
            for (const FFGDroneFuelInformation& FuelInfo : Info->GetDroneFuelInformation())
            {
                if (FuelInfo.FuelItemDescriptor == FuelClass) { Row.FuelPerTrip = FuelInfo.SingleTripFuelCost; break; }
            }
        }

        AFGDroneVehicle* Drone = Station->GetStationDrone();
        if (IsValid(Drone))
        {
            Row.Flags |= EDTTDroneFlags::HasDrone;
            const FDTDHold Cargo = DTDReadHold(Drone->GetStorageInventory());
            Row.CargoItems = Cargo.Items; Row.CargoSlots = Cargo.Slots; Row.CargoCapacity = Cargo.Capacity; Row.CargoFill = Cargo.Fill;

            AFGBuildableDroneStation* Docked = Drone->GetDockedStation();
            if (Docked == Station) Row.Place = EDTTDronePlace::Home;
            else if (IsValid(Docked)) Row.Place = EDTTDronePlace::AtPair;
            else Row.Place = Drone->GetCurrentDestinationStation() == Station ? EDTTDronePlace::ToHome : EDTTDronePlace::ToPair;

            // A drone that cannot unload stands in the port until the belts make room. At a port
            // that only receives, that is a full buffer and nothing is wrong; at one that also sends,
            // the cargo waiting to leave waits with it.
            if (Info->GetDroneStatus() == EDroneStatus::EDS_CANNOT_UNLOAD && IsValid(Docked) &&
                (DTDHasItems(Docked->GetInputInventory()) || DTDHasBelt(Docked, EFactoryConnectionDirection::FCD_INPUT)))
            {
                Row.Flags |= EDTTDroneFlags::UnloadBlocksExport;
            }

            Row.RoundTripSeconds = FMath::RoundToInt(Info->GetAverageRoundTripTime());
            Row.TripItemsIn = Info->GetLatestTripIncomingItemAmount();
            Row.TripItemsOut = Info->GetLatestTripOutgoingItemAmount();
        }
    }

    OutRows.Sort([](const FDTTDroneRow& A, const FDTTDroneRow& B)
    {
        const int32 ByName = A.Name.Compare(B.Name, ESearchCase::IgnoreCase);
        // Two ports may carry one name; the actor keeps their order from flickering between refreshes.
        return ByName != 0 ? ByName < 0 : GetNameSafe(A.Port) < GetNameSafe(B.Port);
    });
}

FString UDynamicTimeTableSubsystem::DroneStatusLabel(uint8 Status)
{
    switch (EDroneStatus(Status))
    {
    case EDroneStatus::EDS_NO_DRONE: return TEXT("No drone");
    case EDroneStatus::EDS_DOCKED: return TEXT("Docked");
    case EDroneStatus::EDS_LOADING: return TEXT("Loading");
    case EDroneStatus::EDS_TAKEOFF: return TEXT("Takeoff");
    case EDroneStatus::EDS_EN_ROUTE: return TEXT("En route");
    case EDroneStatus::EDS_DOCKING: return TEXT("Docking");
    case EDroneStatus::EDS_UNLOADING: return TEXT("Unloading");
    case EDroneStatus::EDS_NOT_ENOUGH_FUEL: return TEXT("No fuel");
    case EDroneStatus::EDS_CANNOT_UNLOAD: return TEXT("Cannot unload");
    default: return TEXT("?");
    }
}

bool UDynamicTimeTableSubsystem::DroneRowMatches(const FDTTDroneRow& Row, const FString& Filter)
{
    if (Filter.IsEmpty() || Row.Name.Contains(Filter) || Row.PairedName.Contains(Filter)) return true;
    for (const TArray<FDTTItemAmount>* Items : { &Row.ExportItems, &Row.ImportItems, &Row.CargoItems })
    {
        for (const FDTTItemAmount& Line : *Items)
        {
            UClass* ItemClass = Line.ItemClass.Get();
            if (ItemClass && UFGItemDescriptor::GetItemName(ItemClass).ToString().Contains(Filter)) return true;
        }
    }
    return false;
}

FString UDynamicTimeTableSubsystem::FormatDroneRow(const FDTTDroneRow& Row)
{
    const bool bDrone = (Row.Flags & EDTTDroneFlags::HasDrone) != 0;
    FString Line = FString::Printf(TEXT("%s | %s%s | "), *Row.Name, *DroneStatusLabel(Row.Status),
        (Row.Flags & EDTTDroneFlags::UnloadBlocksExport) ? TEXT(", holds up what waits to leave") : TEXT(""));
    Line += Row.PairedName.IsEmpty()
        ? (Row.InLinks > 0 ? FString::Printf(TEXT("< %d port(s)"), Row.InLinks) : FString(TEXT("no destination")))
        : FString::Printf(TEXT("> %s %d m"), *Row.PairedName, Row.DistanceMeters);
    Line += FString::Printf(TEXT(" | sends %d/%d [%s] | receives %d/%d [%s]"),
        Row.ExportSlots, Row.ExportCapacity, *DTDItemList(Row.ExportItems),
        Row.ImportSlots, Row.ImportCapacity, *DTDItemList(Row.ImportItems));
    if (bDrone)
    {
        Line += FString::Printf(TEXT(" | drone %d/%d [%s]"), Row.CargoSlots, Row.CargoCapacity, *DTDItemList(Row.CargoItems));
    }
    if (UClass* FuelClass = Row.FuelClass.Get())
    {
        Line += FString::Printf(TEXT(" | fuel %s %d"), *UFGItemDescriptor::GetItemName(FuelClass).ToString(), Row.FuelCount);
        if (Row.FuelPerTrip > 0.0f) Line += FString::Printf(TEXT(" (%.1f a trip)"), Row.FuelPerTrip);
    }
    return Line;
}

void UDynamicTimeTableSubsystem::GetDroneStatusLines(TArray<FString>& OutLines, const FString& Filter) const
{
    OutLines.Reset();
    TArray<FDTTDroneRow> Rows;
    BuildDroneRows(Rows);

    int32 WithDrone = 0, NoFuel = 0, Stuck = 0;
    for (const FDTTDroneRow& Row : Rows)
    {
        if (Row.Flags & EDTTDroneFlags::HasDrone) ++WithDrone;
        if (EDroneStatus(Row.Status) == EDroneStatus::EDS_NOT_ENOUGH_FUEL) ++NoFuel;
        if (EDroneStatus(Row.Status) == EDroneStatus::EDS_CANNOT_UNLOAD) ++Stuck;
    }
    OutLines.Add(FString::Printf(TEXT("dTT drones: ports=%d drones=%d cannot-unload=%d no-fuel=%d"), Rows.Num(), WithDrone, Stuck, NoFuel));
    for (const FDTTDroneRow& Row : Rows)
    {
        if (DroneRowMatches(Row, Filter)) OutLines.Add(FormatDroneRow(Row));
    }
    if (Rows.Num() == 0) OutLines.Add(TEXT("No drone ports in this world."));
}
