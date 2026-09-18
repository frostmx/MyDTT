#pragma once

#include "CoreMinimal.h"
#include "UObject/SoftObjectPtr.h"
#include "FGFactoryColoringTypes.h"
#include "DynamicTimeTableTypes.generated.h"

class AFGTrainStationIdentifier;
class AFGDockingStationIdentifier;
class AFGWheeledVehicleIdentifier;

/** Kind of vehicle a station group balances. A group never mixes kinds: trains and trucks travel different networks. */
UENUM(BlueprintType)
enum class EDTTGroupKind : uint8
{
    // Value 0 so that groups loaded from a pre-truck save stay train groups.
    Train = 0 UMETA(DisplayName = "Trains"),
    Truck = 1 UMETA(DisplayName = "Trucks"),
};

USTRUCT(BlueprintType)
struct FDTTStationEntry
{
    GENERATED_BODY()

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, SaveGame, Category = "Dynamic Time Table")
    FString DisplayName;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Dynamic Time Table", meta = (ClampMin = "1"))
    int32 TargetSlots = 1;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, SaveGame, Category = "Dynamic Time Table")
    int32 PriorityIndex = 0;

    // Persistent identity; DisplayName is only a live display cache.
    UPROPERTY(SaveGame)
    TObjectPtr<AFGTrainStationIdentifier> StationIdentifier = nullptr;

    // Truck groups use these two instead. The path node GUID is the canonical identity: it is
    // stable, it survives a rename, and it is what a vehicle route is actually made of.
    UPROPERTY(SaveGame)
    TObjectPtr<AFGDockingStationIdentifier> DockingStationIdentifier = nullptr;

    UPROPERTY(SaveGame)
    FGuid DockingPathNodeGuid;
};

USTRUCT(BlueprintType)
struct FDTTStationGroup
{
    GENERATED_BODY()

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, SaveGame, Category = "Dynamic Time Table")
    FGuid GroupId;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Dynamic Time Table")
    FString GroupName;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, SaveGame, Category = "Dynamic Time Table")
    EDTTGroupKind Kind = EDTTGroupKind::Train;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, SaveGame, Category = "Dynamic Time Table")
    TArray<FDTTStationEntry> Stations;
};

/** How a docking station's dispatch tag determines its role. */
UENUM(BlueprintType)
enum class EDTTDispatchRole : uint8
{
    None = 0,
    Producer = 1 UMETA(DisplayName = "Producer"),
    Requester = 2 UMETA(DisplayName = "Requester"),
    ByStationMode = 3 UMETA(DisplayName = "LOAD / UNLOAD"),
};

/**
 * A docking station tagged for resource dispatch. Rebuilt wholesale from live station names
 * alongside FDTTStationGroup, same as the rest of this mod's config - nothing here is SaveGame.
 */
USTRUCT(BlueprintType)
struct FDTTDispatchStation
{
    GENERATED_BODY()

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Dynamic Time Table")
    FString DisplayName;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Dynamic Time Table")
    FString ResourceId;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Dynamic Time Table")
    EDTTDispatchRole Role = EDTTDispatchRole::None;

    // Requester only: the N of a "(ResourceId-N)" tag, INDEX_NONE for a neutral "(ResourceId)" tag.
    // The request limit: a solid dock is called while its stock is below N stacks, a fluid dock
    // while it is below N percent of its tank; without N the settings' limits apply. The delivery
    // fills up to the limit plus the buffer (settings) - see GetRequestLevels.
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Dynamic Time Table")
    int32 Threshold = INDEX_NONE;

    // Requester only: the "=" of a "(ResourceId=)" tag. The truck stands at this station, draining
    // into it and topping it up, until its cargo is gone - what the global StayUntilEmpty setting
    // asks of every requester, asked here by one station. A station that stops consuming keeps the
    // truck, which is why this is per station and off unless the name says otherwise.
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Dynamic Time Table")
    bool bStayUntilEmpty = false;

    UPROPERTY()
    TObjectPtr<AFGDockingStationIdentifier> DockingStationIdentifier = nullptr;

    UPROPERTY()
    FGuid DockingPathNodeGuid;
};

/**
 * Remembered producer/optional explicit fuel anchors for one dispatch truck. SaveGame because a lap
 * that does not need the truck's own (F) leaves it out of mVehicleRoute, so the route alone cannot
 * bring it back after a load.
 */
USTRUCT()
struct FDTTDispatchTruckAnchors
{
    GENERATED_BODY()

    UPROPERTY(SaveGame)
    FGuid ProducerGuid;

    // The truck's own (F): the one the player put in its route. Sticky until the station loses
    // the tag or Route[0] changes; included in a lap only when the tank needs it.
    UPROPERTY(SaveGame)
    FGuid FuelGuid;

    // Position of the explicitly tagged (F) stop in the route where it was discovered.
    // Clamped to the rebuilt route size when the number of requesters changes.
    UPROPERTY(SaveGame)
    int32 FuelInsertIndex = INDEX_NONE;

    // (F) borrowed for the current lap because the truck has no own stop and the tank was running
    // critically low. Decided afresh every lap and never adopted as FuelGuid: kept apart so that a
    // borrowed stop still sitting in the route is not mistaken for one the player tagged.
    UPROPERTY(SaveGame)
    FGuid BorrowedFuelGuid;

    // (F) inserted mid-lap by the leg guard (EnsureFuelForNextLeg) because the tank could not
    // cover the next leg twice over. Kept apart from BorrowedFuelGuid: one lap can hold both a
    // planned stop and an emergency one. Never adopted as FuelGuid; cleared at the next boundary.
    UPROPERTY(SaveGame)
    FGuid EmergencyFuelGuid;

    // Set when the player saved this truck's route by hand: the next producer boundary re-reads the
    // own (F) and its position from the route. Otherwise the anchors are authoritative and the route
    // is just what this mod wrote last lap. Runtime only.
    UPROPERTY()
    bool bRouteEditedByPlayer = false;

    // Requester dock GUID -> items this lap plans to leave there (deficit at planning time).
    // Diagnostic: the transfer cap itself is recomputed from the live stock when the truck docks.
    // Saved with the lap: after a reload the last requester of a lap planned before the save
    // still drains (IsLastPlannedRequester), instead of one lap of leftovers riding on.
    UPROPERTY(SaveGame)
    TMap<FGuid, int32> PlannedItems;
};

/**
 * One truck's own paint, kept while this mod wears its cargo colour instead.
 *
 * The game saves the vehicle's current customization itself, so after a load the truck comes back
 * in the cargo colour; what it was before that is only here.
 */
USTRUCT()
struct FDTTSavedTruckPaint
{
    GENERATED_BODY()

    UPROPERTY(SaveGame)
    TObjectPtr<class AFGWheeledVehicleIdentifier> Vehicle = nullptr;

    UPROPERTY(SaveGame)
    FFactoryCustomizationData Original;
};

/** One dispatch truck's anchors as stored by ADTTSaveDataActor. */
USTRUCT()
struct FDTTSavedTruckAnchors
{
    GENERATED_BODY()

    UPROPERTY(SaveGame)
    TObjectPtr<class AFGWheeledVehicleIdentifier> Vehicle = nullptr;

    UPROPERTY(SaveGame)
    FDTTDispatchTruckAnchors Anchors;
};

USTRUCT(BlueprintType)
struct FDTTStationRuntimeLoad
{
    GENERATED_BODY()

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Dynamic Time Table")
    FString StationName;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Dynamic Time Table")
    int32 AssignedTrains = 0;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Dynamic Time Table")
    int32 TargetSlots = 1;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Dynamic Time Table")
    float Ratio = 0.0f;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Dynamic Time Table")
    bool bOverloaded = false;

    /**
     * Truck groups only: this docking station is not on the path network the group's vehicles
     * travel, so the autopilot cannot reach it. Purely diagnostic - the station is still picked
     * as a target, because hiding it would hide the misconfiguration from the player.
     */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Dynamic Time Table")
    bool bUnreachable = false;
};

/** Where a pool truck is in its life. Unmanaged is never stored: such a truck has no entry. */
UENUM(BlueprintType)
enum class EDTTTruckPhase : uint8
{
    Unmanaged,
    // Heading for its depot on a [Depot, Depot] route; parks on arrival.
    ToDepot,
    // Docked at its depot and held there: the station's undock is cancelled until a request comes.
    Parked,
    // Running a dispatched lap [Producer, Requesters..., (F)?, Depot].
    Assigned,
    // The player switched the autopilot off or saved a route by hand; nothing is written until
    // the truck next docks at a depot.
    Manual
};

/**
 * One shared-pool truck. A truck joins the pool when its route contains a (D) depot and leaves
 * it when a player edit removes every depot. SaveGame so a truck saved while parked comes back
 * parked: the game itself saves the docked actor and the docking flag, only the hold is ours.
 */
USTRUCT(BlueprintType)
struct FDTTPoolTruckState
{
    GENERATED_BODY()

    UPROPERTY(SaveGame, BlueprintReadOnly, Category = "Dynamic Time Table")
    EDTTTruckPhase Phase = EDTTTruckPhase::Unmanaged;

    /** The depot claimed by this truck: where it is parked, or where it is heading. */
    UPROPERTY(SaveGame, BlueprintReadOnly, Category = "Dynamic Time Table")
    FGuid DepotGuid;

    /** Resource of the current or last lap. */
    UPROPERTY(SaveGame, BlueprintReadOnly, Category = "Dynamic Time Table")
    FString ResourceId;

    UPROPERTY(SaveGame)
    FGuid ProducerGuid;

    UPROPERTY(SaveGame)
    TArray<FGuid> Requesters;

    UPROPERTY(SaveGame)
    FGuid FuelGuid;

    /** (F) the leg guard inserted mid-lap (EnsureFuelForNextLeg). Cleared by the next assignment. */
    UPROPERTY(SaveGame)
    FGuid EmergencyFuelGuid;

    /** True while the station hold is (or, right after a load, was) in place. */
    UPROPERTY(SaveGame, BlueprintReadOnly, Category = "Dynamic Time Table")
    bool bParked = false;

    /** Requester dock GUID -> items this lap planned to leave there. Saved with the lap. */
    UPROPERTY(SaveGame)
    TMap<FGuid, int32> PlannedItems;

    /** World seconds of the last assignment. Runtime only. */
    double AssignedAtSeconds = 0.0;
    /** POOL_CARGO_WAIT was logged for this parking. Runtime only, cleared by ParkTruck. */
    bool bCargoWaitLogged = false;
    /**
     * The last requester of the current lap, once the truck has left it: while valid and the phase
     * is Assigned, the dispatcher may start the next lap from there instead of the depot the truck
     * is driving back to. Runtime only, cleared by ParkTruck and by the next assignment.
     */
    FGuid ChainFromGuid;
};

/** One pool truck as stored by ADTTSaveDataActor. */
USTRUCT()
struct FDTTSavedPoolTruck
{
    GENERATED_BODY()

    UPROPERTY(SaveGame)
    TObjectPtr<class AFGWheeledVehicleIdentifier> Vehicle = nullptr;

    UPROPERTY(SaveGame)
    FDTTPoolTruckState State;
};

/** One line of the Dispatch tab / "/dtt dispatch": a tagged station, a depot or a fuel stop. */
USTRUCT(BlueprintType)
struct FDTTDispatchRow
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") FString ResourceId;
    /** The item the resource resolved to, for its icon; unset when nothing resolved. */
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") TSoftClassPtr<class UFGItemDescriptor> ItemClass;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") FString StationName;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") FGuid DockingPathNodeGuid;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") EDTTDispatchRole Role = EDTTDispatchRole::None;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") bool bDepot = false;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") bool bFuelStop = false;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") bool bModeKnown = false;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") bool bLoadMode = false;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") bool bModeMatches = false;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") int32 StockStacks = -1;
    /** Requester: called while the stock is below this many stacks. */
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") int32 TriggerStacks = -1;
    /** Requester: a delivery fills it up to this many stacks (the limit plus the buffer). */
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") int32 ThresholdStacks = -1;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") int32 CapacityStacks = -1;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") bool bFluid = false;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") bool bOpen = false;
    /**
     * The dock itself, for the window's row buttons. The identifier is replicated to every
     * client at all times, unlike the station, so a row can act on its dock wherever it is on
     * the map. Null only while the client has yet to resolve it, which greys the row's buttons.
     */
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") TObjectPtr<AFGDockingStationIdentifier> Dock = nullptr;
    /** Requester: the truck holding the claim; depot: the truck parked or heading there. Empty when free. */
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") FString ClaimOwner;
    /** ClaimOwner's place in the snapshot's truck-name table; see FDTTClientSnapshot. */
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") int32 ClaimTruckId = INDEX_NONE;
    /** Requester: stacks the claiming truck's lap plans to leave here. */
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") int32 PlannedStacks = 0;
    /** Requester: how long it has been open with nobody claiming it; 0 when closed or claimed. */
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") int32 WaitingSeconds = 0;
    /** Depot: docked truck and hold; fuel stop: the guid. */
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") FString Note;
};

/** One line of the Trucks tab / "/dtt trucks": a legacy dispatch truck or a pool truck. */
USTRUCT(BlueprintType)
struct FDTTTruckRow
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") FString Name;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") bool bPool = false;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") EDTTTruckPhase Phase = EDTTTruckPhase::Unmanaged;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") FString ResourceId;
    /** The truck's resource and what is actually on board (when it is one item), for the icons. */
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") TSoftClassPtr<class UFGItemDescriptor> ItemClass;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") TSoftClassPtr<class UFGItemDescriptor> CargoItemClass;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") TArray<FString> RouteStops;
    /** The same stops as places in the snapshot's dock-name table; see FDTTClientSnapshot. */
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") TArray<int32> RouteStopIds;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") int32 TargetIndex = -1;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") int32 CargoStacks = -1;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") int32 CargoCapacityStacks = -1;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") FString DepotName;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") FString PlannedSummary;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") bool bAutopilot = false;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") bool bDocking = false;
    /** What is on board by item, and why that matters ("waits for Plastic", "foreign cargo"). */
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") FString CargoNote;
    /** The truck itself, for the window's row buttons; see FDTTDispatchRow::Dock. */
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") TObjectPtr<AFGWheeledVehicleIdentifier> Truck = nullptr;
};

/**
 * One docking station on the Stations tab: everything a table of the whole map needs, and
 * nothing that costs a string. A world with four hundred docks would put sixty-odd KB of
 * names on the wire against a 64 KB call, so the two names ride as places in the snapshot's
 * dock-name table and the flags ride in one byte.
 */
USTRUCT(BlueprintType)
struct FDTTStationRow
{
    GENERATED_BODY()

    /** Places in FDTTClientSnapshot::DockNames; -1 for a name there is none of. */
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") int32 NameId = INDEX_NONE;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") int32 ResourceNameId = INDEX_NONE;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") int32 GroupNameId = INDEX_NONE;
    /** Place in FDTTClientSnapshot::TruckNames: who holds the request, or -1 for nobody. */
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") int32 ClaimTruckId = INDEX_NONE;
    /** The names themselves on a listen host, where nothing is sent and nothing is interned. */
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") FString Name;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") FString ResourceId;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") FString GroupName;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") FString ClaimOwner;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") TSoftClassPtr<class UFGItemDescriptor> ItemClass;
    /** The dock itself, for the row's controls; see FDTTDispatchRow::Dock. */
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") TObjectPtr<AFGDockingStationIdentifier> Dock = nullptr;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") FGuid DockingPathNodeGuid;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") EDTTDispatchRole Role = EDTTDispatchRole::None;
    /**
     * The limit as the name spells it, not as the dispatcher works it out: this is the number
     * the row edits, and writing back an effective value would bake a default into the name.
     * 0 when the tag carries none.
     */
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") int32 TagThreshold = 0;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") int32 StockStacks = -1;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") int32 TriggerStacks = -1;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") int32 ThresholdStacks = -1;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") int32 CapacityStacks = -1;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") int32 WaitingSeconds = 0;
    /** EDTTStationFlags; one byte instead of eight bools, which is 7 bytes a row. */
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") uint8 Flags = 0;
};

/** The bits of FDTTStationRow::Flags. */
namespace EDTTStationFlags
{
    enum Type : uint8
    {
        None        = 0,
        ModeKnown   = 1 << 0,
        LoadMode    = 1 << 1,
        ModeMatches = 1 << 2,
        Fluid       = 1 << 3,
        Depot       = 1 << 4,
        FuelStop    = 1 << 5,
        Stay        = 1 << 6,
        Open        = 1 << 7,
    };
}

/**
 * Why a pool pass did not send a truck. These mirror the POOL_SKIP reasons the dispatcher
 * already logs, one for one - the counts are recorded where those lines are written rather than
 * worked out again later, because working them out again means a path search per resource on
 * every refresh, and because two answers to "why is nothing moving" would be one too many.
 */
UENUM(BlueprintType)
enum class EDTTSkipReason : uint8
{
    None = 0,
    /** The resource in the tag resolves to no item: a typo, or a language the world does not use. */
    ItemClassNotFound,
    /** Solid truck against a fluid station, or the other way round. */
    Form,
    /** The truck is carrying something else. */
    ForeignCargo,
    /** The lap would start at a consumer and the truck has nothing on board. */
    NoCargo,
    /** No vehicle path from where the truck is to the station. */
    Unreachable,
    /** Nobody is producing it, or the producers are empty. */
    NoProducerWithStock,
    /** There is stock, but no free truck that fits. */
    NoSuitableTruck,
};

/** One truck's lap: where it is going, along which stops, and how far is left of them. */
USTRUCT(BlueprintType)
struct FDTTLapRow
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") int32 TruckNameId = INDEX_NONE;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") int32 ResourceNameId = INDEX_NONE;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") FString TruckName;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") FString ResourceId;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") TSoftClassPtr<class UFGItemDescriptor> ItemClass;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") TObjectPtr<AFGWheeledVehicleIdentifier> Truck = nullptr;
    /** The stops of the lap, as places in the snapshot's dock-name table. */
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") TArray<int32> StopIds;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") TArray<FString> RouteStops;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") int32 TargetIndex = INDEX_NONE;
    /**
     * Metres of road left to the end of this lap, along the path the truck will actually drive.
     * -1 when no path can be costed.
     */
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") int32 DistanceLeftMetres = -1;
    /**
     * Metres of road left to the stop the truck is heading for: the straight line to it, stretched
     * by how much longer than its straight line the road of that leg is. -1 when unknown.
     */
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") int32 NextStopMetres = -1;
    /**
     * Average speed over the last minute, km/h, from a sample every five seconds; -1 before the
     * first sample. An average and not the speedometer: a reading taken at refresh time jumps
     * with every corner and every junction, and the window would blink with it.
     */
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") int32 SpeedKmh = -1;
    /**
     * Seconds to the stop the truck is heading for: NextStopMetres over the average of the last
     * minute of driving, which leaves out the samples taken while docked. SpeedKmh would not do:
     * a truck that has just left a dock carries a minute of standing in it and was promised
     * arrivals up to three times too late. -1 when there is no road length or no driving yet.
     */
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") int32 NextStopSeconds = -1;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") int32 CargoStacks = -1;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") int32 CargoCapacityStacks = -1;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") EDTTTruckPhase Phase = EDTTTruckPhase::Unmanaged;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") bool bAutopilot = false;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") bool bDocking = false;
};

/** One open request, in the order the dispatcher will look at them. */
USTRUCT(BlueprintType)
struct FDTTQueueRow
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") int32 NameId = INDEX_NONE;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") int32 ResourceNameId = INDEX_NONE;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") int32 ClaimTruckId = INDEX_NONE;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") FString Name;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") FString ResourceId;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") FString ClaimOwner;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") TSoftClassPtr<class UFGItemDescriptor> ItemClass;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") TObjectPtr<AFGDockingStationIdentifier> Dock = nullptr;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") FGuid DockingPathNodeGuid;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") int32 WaitingSeconds = 0;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") int32 StockStacks = -1;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") int32 TriggerStacks = -1;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") int32 CapacityStacks = -1;
};

/** How often one reason stopped one resource in the dispatcher's last pass. */
USTRUCT(BlueprintType)
struct FDTTReasonRow
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") int32 ResourceNameId = INDEX_NONE;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") FString ResourceId;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") EDTTSkipReason Reason = EDTTSkipReason::None;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") int32 Count = 0;
    /** Open requests of that resource when the pass ran. */
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") int32 OpenRequests = 0;
};

/**
 * The tunables. Saved with the world and edited from the Settings tab of the window (or
 * "/dtt set"); the server's copy is the one that counts, a remote client only proposes.
 */
USTRUCT(BlueprintType)
struct FDTTSettings
{
    GENERATED_BODY()

    /**
     * On: the loading station gives a lap only its requesters' shortfall, a requester takes only up
     * to its target and the last one drains what is left. Off (default): every docking moves what
     * the game would - a full load at the producer, then everything that fits at each requester;
     * a truck that empties early starts its next lap from where it is.
     */
    UPROPERTY(SaveGame, BlueprintReadWrite, Category = "Dynamic Time Table") bool bExactQuantities = false;
    /** A solid requester without a number in its tag is called while its stock is below this many stacks (clamped to the dock). */
    UPROPERTY(SaveGame, BlueprintReadWrite, Category = "Dynamic Time Table") int32 SolidRequestStacks = 40;
    /** A fluid requester without a number in its tag is called while its tank holds fewer than this many stacks of 50 000 L. */
    UPROPERTY(SaveGame, BlueprintReadWrite, Category = "Dynamic Time Table") int32 FluidRequestStacks = 56;
    /** A delivery fills a requester up to its limit plus this many stacks, so what is consumed on the way is covered. */
    UPROPERTY(SaveGame, BlueprintReadWrite, Category = "Dynamic Time Table") int32 RequestBufferStacks = 8;
    /** A fluid lap serves one requester in either mode; with exact quantities the producer also fills the tank completely and the leftovers drain at that requester. */
    UPROPERTY(SaveGame, BlueprintReadWrite, Category = "Dynamic Time Table") bool bFluidFullLoad = true;
    /** The last requester of a lap takes everything it can in its docking cycle, target or not, and with LeftoverHoldSeconds > 0 the truck stays and tops it up. Works in either quantities mode; with exact quantities off the hold is the whole difference. Off: the truck keeps its leftovers. */
    UPROPERTY(SaveGame, BlueprintReadWrite, Category = "Dynamic Time Table") bool bDrainLeftovers = true;
    /** Extra seconds the drained truck stays docked at that last requester, topping it up as it consumes. 0 (default): it leaves right after the unload. */
    UPROPERTY(SaveGame, BlueprintReadWrite, Category = "Dynamic Time Table") float LeftoverHoldSeconds = 0.0f;
    /** Off (default): a lap visits its requesters one after the other. On: the truck stays docked at every requester of its lap, topping it up as it consumes, until its cargo is gone - then it drives on. A requester that stops consuming keeps the truck; only a full tank on board and no demand at all ever frees it. */
    UPROPERTY(SaveGame, BlueprintReadWrite, Category = "Dynamic Time Table") bool bStayUntilEmpty = false;
    /** Seconds between two top-up transfers of a held truck (at a requester or a depot). The poll runs once a second, so 1 is the floor. */
    UPROPERTY(SaveGame, BlueprintReadWrite, Category = "Dynamic Time Table") float TopUpIntervalSeconds = 1.0f;
    /** A parked truck unloads everything into its depot when the depot is in UNLOAD mode. */
    UPROPERTY(SaveGame, BlueprintReadWrite, Category = "Dynamic Time Table") bool bUnloadAtDepot = true;
    /** A pool lap skips the producer when the cargo already on board covers every planned deficit. */
    UPROPERTY(SaveGame, BlueprintReadWrite, Category = "Dynamic Time Table") bool bSkipProducerWhenCargoCovers = true;
    /** Seconds between two timer passes of the pool dispatcher. */
    UPROPERTY(SaveGame, BlueprintReadWrite, Category = "Dynamic Time Table") float PoolDispatchIntervalSeconds = 5.0f;
    /** Upper bound on requesters planned into one lap. */
    UPROPERTY(SaveGame, BlueprintReadWrite, Category = "Dynamic Time Table") int32 MaxRequestersPerLap = 4;
    /** Seconds after which an unserved requester claim is dropped. */
    UPROPERTY(SaveGame, BlueprintReadWrite, Category = "Dynamic Time Table") float RequesterClaimTimeoutSeconds = 600.0f;
    /** An open request nobody has claimed for this many minutes is shown in red; 0 turns it off. */
    UPROPERTY(SaveGame, BlueprintReadWrite, Category = "Dynamic Time Table") int32 OverdueRequestMinutes = 5;
    /** Seconds between re-evaluations of a dispatch truck idling on a one-stop route. */
    UPROPERTY(SaveGame, BlueprintReadWrite, Category = "Dynamic Time Table") float IdleDispatchRecheckSeconds = 10.0f;
    /** A truck borrows a fuel stop below this many estimated lap costs of fuel. */
    UPROPERTY(SaveGame, BlueprintReadWrite, Category = "Dynamic Time Table") float FuelCriticalMarginLaps = 2.0f;
    /** A truck leaves its own (F) out of a lap above this many estimated lap costs of fuel. */
    UPROPERTY(SaveGame, BlueprintReadWrite, Category = "Dynamic Time Table") float FuelSkipMarginLaps = 2.5f;
    /** The leg guard demands this many times the estimated cost of the next leg in the tank before letting the truck drive it; below that a fuel stop goes in first. The engine's per-segment estimate is unverified against real consumption, so 1.0 is not a safe value. */
    UPROPERTY(SaveGame, BlueprintReadWrite, Category = "Dynamic Time Table") float FuelLegMarginFactor = 2.0f;
    /** A borrowed (F) is only placed where the tank can reach it using at most this fraction of its current energy. */
    UPROPERTY(SaveGame, BlueprintReadWrite, Category = "Dynamic Time Table") float FuelReachFraction = 0.8f;
    /**
     * Off by default on purpose: a world that already has hand-written tags must look exactly the
     * same after the update. On, the mod reads the belts around every dock, writes what it found
     * into the station's name and sets its LOAD/UNLOAD switch.
     */
    UPROPERTY(SaveGame, BlueprintReadWrite, Category = "Dynamic Time Table") bool bAutoNameStations = false;
    /** Quiet time after the last build or rename before a dirty station is walked again. */
    UPROPERTY(SaveGame, BlueprintReadWrite, Category = "Dynamic Time Table") float AutoNameDebounceSeconds = 2.0f;
    /** Every station is marked dirty this often, which is what catches an overclock change. */
    UPROPERTY(SaveGame, BlueprintReadWrite, Category = "Dynamic Time Table") float AutoNameSweepSeconds = 60.0f;
    /** Ceiling on the buildings one walk may visit; above it the station is left alone. */
    UPROPERTY(SaveGame, BlueprintReadWrite, Category = "Dynamic Time Table") int32 AutoNameMaxNodes = 500;
    /** How many final products the name lists before it ends in an ellipsis. */
    UPROPERTY(SaveGame, BlueprintReadWrite, Category = "Dynamic Time Table") int32 AutoNameMaxProducts = 3;
    /**
     * The game language item names are written in: "en", or one of the culture codes the options
     * menu offers. A dedicated server has no language of its own and a name is read by everyone,
     * so this is a setting rather than whatever culture the process happens to run in.
     */
    UPROPERTY(SaveGame, BlueprintReadWrite, Category = "Dynamic Time Table") FString NamingLanguage = TEXT("en");

    void Sanitize()
    {
        NamingLanguage = NamingLanguage.TrimStartAndEnd();
        if (NamingLanguage.IsEmpty()) NamingLanguage = TEXT("en");
        SolidRequestStacks = FMath::Clamp(SolidRequestStacks, 0, 200);
        FluidRequestStacks = FMath::Clamp(FluidRequestStacks, 0, 200);
        RequestBufferStacks = FMath::Clamp(RequestBufferStacks, 0, 64);
        LeftoverHoldSeconds = FMath::Clamp(LeftoverHoldSeconds, 0.0f, 600.0f);
        TopUpIntervalSeconds = FMath::Clamp(TopUpIntervalSeconds, 1.0f, 30.0f);
        PoolDispatchIntervalSeconds = FMath::Clamp(PoolDispatchIntervalSeconds, 1.0f, 60.0f);
        MaxRequestersPerLap = FMath::Clamp(MaxRequestersPerLap, 1, 8);
        RequesterClaimTimeoutSeconds = FMath::Clamp(RequesterClaimTimeoutSeconds, 30.0f, 3600.0f);
        OverdueRequestMinutes = FMath::Clamp(OverdueRequestMinutes, 0, 120);
        IdleDispatchRecheckSeconds = FMath::Clamp(IdleDispatchRecheckSeconds, 1.0f, 120.0f);
        FuelCriticalMarginLaps = FMath::Clamp(FuelCriticalMarginLaps, 1.0f, 10.0f);
        // Keep the dead zone between "borrow" and "safe to skip" (see the 1.0.2 story).
        FuelSkipMarginLaps = FMath::Clamp(FuelSkipMarginLaps, FuelCriticalMarginLaps + 0.1f, 10.0f);
        FuelLegMarginFactor = FMath::Clamp(FuelLegMarginFactor, 1.0f, 5.0f);
        FuelReachFraction = FMath::Clamp(FuelReachFraction, 0.3f, 1.0f);
        AutoNameDebounceSeconds = FMath::Clamp(AutoNameDebounceSeconds, 0.5f, 30.0f);
        AutoNameSweepSeconds = FMath::Clamp(AutoNameSweepSeconds, 10.0f, 600.0f);
        AutoNameMaxNodes = FMath::Clamp(AutoNameMaxNodes, 50, 5000);
        AutoNameMaxProducts = FMath::Clamp(AutoNameMaxProducts, 1, 8);
    }
    bool operator==(const FDTTSettings& Other) const
    {
        return bExactQuantities == Other.bExactQuantities &&
            SolidRequestStacks == Other.SolidRequestStacks && FluidRequestStacks == Other.FluidRequestStacks &&
            RequestBufferStacks == Other.RequestBufferStacks && bFluidFullLoad == Other.bFluidFullLoad &&
            bDrainLeftovers == Other.bDrainLeftovers && bStayUntilEmpty == Other.bStayUntilEmpty &&
            LeftoverHoldSeconds == Other.LeftoverHoldSeconds && TopUpIntervalSeconds == Other.TopUpIntervalSeconds &&
            bUnloadAtDepot == Other.bUnloadAtDepot && bSkipProducerWhenCargoCovers == Other.bSkipProducerWhenCargoCovers &&
            PoolDispatchIntervalSeconds == Other.PoolDispatchIntervalSeconds && MaxRequestersPerLap == Other.MaxRequestersPerLap &&
            RequesterClaimTimeoutSeconds == Other.RequesterClaimTimeoutSeconds && OverdueRequestMinutes == Other.OverdueRequestMinutes &&
            IdleDispatchRecheckSeconds == Other.IdleDispatchRecheckSeconds &&
            FuelCriticalMarginLaps == Other.FuelCriticalMarginLaps && FuelSkipMarginLaps == Other.FuelSkipMarginLaps &&
            FuelLegMarginFactor == Other.FuelLegMarginFactor && FuelReachFraction == Other.FuelReachFraction &&
            bAutoNameStations == Other.bAutoNameStations && AutoNameDebounceSeconds == Other.AutoNameDebounceSeconds &&
            AutoNameSweepSeconds == Other.AutoNameSweepSeconds && AutoNameMaxNodes == Other.AutoNameMaxNodes &&
            AutoNameMaxProducts == Other.AutoNameMaxProducts && NamingLanguage == Other.NamingLanguage;
    }
    bool operator!=(const FDTTSettings& Other) const { return !(*this == Other); }
    FString Describe() const
    {
        return FString::Printf(TEXT("ExactQuantities=%s SolidRequestStacks=%d FluidRequestStacks=%d RequestBufferStacks=%d FluidFullLoad=%s DrainLeftovers=%s "
            "LeftoverHoldSeconds=%.0f StayUntilEmpty=%s TopUpIntervalSeconds=%.0f UnloadAtDepot=%s SkipProducerWhenCargoCovers=%s "
            "PoolDispatchIntervalSeconds=%.0f MaxRequestersPerLap=%d RequesterClaimTimeoutSeconds=%.0f OverdueRequestMinutes=%d "
            "IdleDispatchRecheckSeconds=%.0f FuelCriticalMarginLaps=%.2f FuelSkipMarginLaps=%.2f FuelLegMarginFactor=%.2f FuelReachFraction=%.2f "
            "AutoNameStations=%s AutoNameDebounceSeconds=%.0f AutoNameSweepSeconds=%.0f AutoNameMaxNodes=%d AutoNameMaxProducts=%d NamingLanguage=%s"),
            bExactQuantities ? TEXT("true") : TEXT("false"),
            SolidRequestStacks, FluidRequestStacks, RequestBufferStacks, bFluidFullLoad ? TEXT("true") : TEXT("false"),
            bDrainLeftovers ? TEXT("true") : TEXT("false"),
            LeftoverHoldSeconds, bStayUntilEmpty ? TEXT("true") : TEXT("false"),
            TopUpIntervalSeconds, bUnloadAtDepot ? TEXT("true") : TEXT("false"),
            bSkipProducerWhenCargoCovers ? TEXT("true") : TEXT("false"), PoolDispatchIntervalSeconds, MaxRequestersPerLap,
            RequesterClaimTimeoutSeconds, OverdueRequestMinutes, IdleDispatchRecheckSeconds, FuelCriticalMarginLaps, FuelSkipMarginLaps,
            FuelLegMarginFactor, FuelReachFraction,
            bAutoNameStations ? TEXT("true") : TEXT("false"), AutoNameDebounceSeconds, AutoNameSweepSeconds,
            AutoNameMaxNodes, AutoNameMaxProducts, *NamingLanguage);
    }
    /** "/dtt set <field> <value>": true when the field exists and parsed. */
    bool SetByName(const FString& Field, const FString& Value)
    {
        auto AsFloat = [&](float& Out) { if (!Value.IsNumeric()) return false; Out = FCString::Atof(*Value); return true; };
        auto AsBool = [&](bool& Out)
        {
            if (Value.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Value == TEXT("1") || Value.Equals(TEXT("on"), ESearchCase::IgnoreCase)) { Out = true; return true; }
            if (Value.Equals(TEXT("false"), ESearchCase::IgnoreCase) || Value == TEXT("0") || Value.Equals(TEXT("off"), ESearchCase::IgnoreCase)) { Out = false; return true; }
            return false;
        };
        auto AsInt = [&](int32& Out) { if (!Value.IsNumeric()) return false; Out = FCString::Atoi(*Value); return true; };
        const FString F = Field.ToLower();
        if (F == TEXT("exactquantities")) return AsBool(bExactQuantities);
        if (F == TEXT("solidrequeststacks")) return AsInt(SolidRequestStacks);
        if (F == TEXT("fluidrequeststacks")) return AsInt(FluidRequestStacks);
        if (F == TEXT("requestbufferstacks")) return AsInt(RequestBufferStacks);
        if (F == TEXT("fluidfullload")) return AsBool(bFluidFullLoad);
        if (F == TEXT("drainleftovers")) return AsBool(bDrainLeftovers);
        if (F == TEXT("leftoverholdseconds")) return AsFloat(LeftoverHoldSeconds);
        if (F == TEXT("stayuntilempty")) return AsBool(bStayUntilEmpty);
        if (F == TEXT("topupintervalseconds")) return AsFloat(TopUpIntervalSeconds);
        if (F == TEXT("unloadatdepot")) return AsBool(bUnloadAtDepot);
        if (F == TEXT("skipproducerwhencargocovers")) return AsBool(bSkipProducerWhenCargoCovers);
        if (F == TEXT("pooldispatchintervalseconds")) return AsFloat(PoolDispatchIntervalSeconds);
        if (F == TEXT("maxrequestersperlap")) return AsInt(MaxRequestersPerLap);
        if (F == TEXT("requesterclaimtimeoutseconds")) return AsFloat(RequesterClaimTimeoutSeconds);
        if (F == TEXT("overduerequestminutes")) return AsInt(OverdueRequestMinutes);
        if (F == TEXT("idledispatchrecheckseconds")) return AsFloat(IdleDispatchRecheckSeconds);
        if (F == TEXT("fuelcriticalmarginlaps")) return AsFloat(FuelCriticalMarginLaps);
        if (F == TEXT("fuelskipmarginlaps")) return AsFloat(FuelSkipMarginLaps);
        if (F == TEXT("fuellegmarginfactor")) return AsFloat(FuelLegMarginFactor);
        if (F == TEXT("fuelreachfraction")) return AsFloat(FuelReachFraction);
        if (F == TEXT("autonamestations")) return AsBool(bAutoNameStations);
        if (F == TEXT("autonamedebounceseconds")) return AsFloat(AutoNameDebounceSeconds);
        if (F == TEXT("autonamesweepseconds")) return AsFloat(AutoNameSweepSeconds);
        if (F == TEXT("autonamemaxnodes")) return AsInt(AutoNameMaxNodes);
        if (F == TEXT("autonamemaxproducts")) return AsInt(AutoNameMaxProducts);
        if (F == TEXT("naminglanguage")) { NamingLanguage = Value.TrimStartAndEnd(); return !NamingLanguage.IsEmpty(); }
        return false;
    }
};
