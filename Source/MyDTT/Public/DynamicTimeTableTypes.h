#pragma once

#include "CoreMinimal.h"
#include "DynamicTimeTableTypes.generated.h"

class AFGTrainStationIdentifier;
class AFGDockingStationIdentifier;

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
    // Not a threshold by itself but an offset from the dock's capacity: the requester is open while
    // its stock is below capacity minus N stacks (a neutral tag: below capacity, i.e. not full).
    // The capacity is only known from the live inventory - see GetEffectiveThreshold.
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Dynamic Time Table")
    int32 Threshold = INDEX_NONE;

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

    // Set when the player saved this truck's route by hand: the next producer boundary re-reads the
    // own (F) and its position from the route. Otherwise the anchors are authoritative and the route
    // is just what this mod wrote last lap. Runtime only.
    UPROPERTY()
    bool bRouteEditedByPlayer = false;

    // Requester dock GUID -> items this lap plans to leave there (deficit at planning time).
    // Diagnostic: the transfer cap itself is recomputed from the live stock when the truck docks.
    // Runtime only.
    UPROPERTY()
    TMap<FGuid, int32> PlannedItems;
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

    /** True while the station hold is (or, right after a load, was) in place. */
    UPROPERTY(SaveGame, BlueprintReadOnly, Category = "Dynamic Time Table")
    bool bParked = false;

    /** Requester dock GUID -> items this lap planned to leave there. Runtime only. */
    UPROPERTY()
    TMap<FGuid, int32> PlannedItems;

    /** World seconds of the last assignment. Runtime only. */
    double AssignedAtSeconds = 0.0;
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
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") FString StationName;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") FGuid DockingPathNodeGuid;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") EDTTDispatchRole Role = EDTTDispatchRole::None;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") bool bDepot = false;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") bool bFuelStop = false;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") bool bModeKnown = false;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") bool bLoadMode = false;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") bool bModeMatches = false;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") int32 StockStacks = -1;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") int32 ThresholdStacks = -1;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") int32 CapacityStacks = -1;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") bool bOpen = false;
    /** Requester: the truck holding the claim; depot: the truck parked or heading there. Empty when free. */
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") FString ClaimOwner;
    /** Requester: stacks the claiming truck's lap plans to leave here. */
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") int32 PlannedStacks = 0;
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
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") TArray<FString> RouteStops;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") int32 TargetIndex = -1;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") int32 CargoStacks = -1;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") int32 CargoCapacityStacks = -1;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") FString DepotName;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") FString PlannedSummary;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") bool bAutopilot = false;
    UPROPERTY(BlueprintReadOnly, Category = "Dynamic Time Table") bool bDocking = false;
};
