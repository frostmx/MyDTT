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

    // Requester only: open (a candidate for dispatch) while the dock's stock is below this many stacks.
    // A neutral "(ResourceId)" tag receives the subsystem's default threshold.
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Dynamic Time Table")
    int32 Threshold = 0;

    UPROPERTY()
    TObjectPtr<AFGDockingStationIdentifier> DockingStationIdentifier = nullptr;

    UPROPERTY()
    FGuid DockingPathNodeGuid;
};

/** Remembered producer/optional explicit fuel anchors for one dispatch truck. */
USTRUCT()
struct FDTTDispatchTruckAnchors
{
    GENERATED_BODY()

    UPROPERTY()
    FGuid ProducerGuid;

    UPROPERTY()
    FGuid FuelGuid;

    // Position of the explicitly tagged (F) stop in the route where it was discovered.
    // Clamped to the rebuilt route size when the number of requesters changes.
    UPROPERTY()
    int32 FuelInsertIndex = INDEX_NONE;

    // True when FuelGuid should stay in mVehicleRoute this lap but the truck should bypass it
    // without stopping (SetCurrentTargetWaypoint past it) - decided once per lap, at the producer
    // boundary. Only meaningful for a player-tagged (own-route) fuel stop.
    UPROPERTY()
    bool bSkipFuelThisLap = false;

    // True when FuelGuid was not tagged in this producer's own route but borrowed from the nearest
    // reachable (F) because the tank was running critically low. Unlike a player's own tag, a
    // borrowed stop is not sticky: it is dropped from the route again once no longer needed.
    UPROPERTY()
    bool bFuelIsBorrowed = false;
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
