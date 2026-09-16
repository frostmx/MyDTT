#pragma once

#include "CoreMinimal.h"
#include "Templates/SubclassOf.h"

class AFGBuildableDockingStation;
class UFGItemDescriptor;

/**
 * Bounds on one scan, taken from FDTTSettings. A factory is a graph the player builds, so the
 * walk needs a ceiling it cannot be argued out of.
 */
struct FDTTAutoNameLimits
{
    int32 MaxNodes = 500;
    int32 MaxMachines = 64;
    int32 MaxProducts = 3;

    /** Work out what every signal says, not just the one that decides. For the chat commands. */
    bool bExplainAll = false;
};

/**
 * What the belts and pipes say about one docking station.
 *
 * Derived on the server and never saved: the station's own name is the storage, the same way
 * groups and dispatch tags already work. See the plan in
 * C:\Users\frost\.claude\plans\mydtt-autoname-stations.md.
 *
 * The scanner answers in item classes. Their names are the caller's business, because which
 * language a name is written in is a setting, not a property of the world.
 */
struct FDTTAutoNameResult
{
    bool bValid = false;

    /** true: the station feeds the factory, so a truck brings this resource ('-', UNLOAD mode). */
    bool bRequester = false;

    /** The dock moves fluids through pipes rather than solids over belts. */
    bool bFluidDock = false;

    TSubclassOf<UFGItemDescriptor> ItemClass;

    /** ItemClass in the naming language. Filled by the caller, empty out of the scanner. */
    FString ResourceName;

    float RatePerMinute = 0.0f;

    /**
     * For a collecting station: what the machines behind the belt make in all, before the other
     * branches on the way took their share. RatePerMinute is what reaches the dock.
     */
    float LineRatePerMinute = 0.0f;

    /** The chain wanted more than one item and the biggest one won. Printed as '?'. */
    bool bMixed = false;

    /** The item came from the port a belt lands on, not from cargo or a tag. Also printed as '?'. */
    bool bFromPort = false;

    /** What the ports said, whether or not it was used - kept so the guess can be judged in the log. */
    TSubclassOf<UFGItemDescriptor> PortItem;
    int32 PortVoteCount = 0;

    /** cargo | tag | unsupplied | port | dominant | only-one | held. Printed by /dtt autoname <station>. */
    FString DecidedBy;

    /** The ingredient this chain needs that no belt or pipe brings, when there is exactly one of them. */
    TSubclassOf<UFGItemDescriptor> UnsuppliedItem;
    int32 UnsuppliedCount = 0;

    /**
     * Nothing behind the dock consumes or produces anything: the belt or pipe ends in containers,
     * a fluid buffer or a train platform. Such a station has no rate and is named after what those
     * hold. Machines then lists the stores, so two docks on one container still share a '*'.
     */
    bool bWarehouse = false;
    int32 StoreCount = 0;
    /** How many different items the stores held, so a guess among several can be marked. */
    int32 HeldKinds = 0;

    /** Final products of the chain, biggest flow first. Requester side only. */
    TArray<TSubclassOf<UFGItemDescriptor>> ProductClasses;
    /** ProductClasses in the naming language. Filled by the caller. */
    TArray<FString> Products;
    bool bMoreProducts = false;

    /** Generators at the end of the chain, their capacity summed. Printed as "N MW" after the products. */
    float PowerMW = 0.0f;

    /** Machines (or stores) the chain reaches. Two stations sharing any of them both get the '*' mark. */
    TArray<TWeakObjectPtr<AActor>> Machines;

    /** Why the station was left alone. Empty when bValid. */
    FString SkipReason;
};

/**
 * Walks the belt and pipe network around a docking station and answers three questions: which way
 * the goods travel, which item the chain wants, and how much of it per minute.
 *
 * Which ingredient arrives on a belt cannot be read off the connection at all - neither the port
 * order nor GetInventoryAccessIndex() answers it, measured in the live world on 16.09.2026. A
 * machine is read as wanting all of its ingredients, and the dock's own cargo picks one.
 */
class FDTTAutoNameScanner
{
public:
    /**
     * Server only, game thread. false leaves Out.SkipReason saying why.
     *
     * Preferred is what the station's current tag says, resolved to an item class. It is used only
     * when the dock is empty and the chain wants more than one thing: the player already answered
     * this question once, and guessing differently would rename a working station for no reason.
     */
    static bool Scan(AFGBuildableDockingStation* Station, const FDTTAutoNameLimits& Limits,
        TSubclassOf<UFGItemDescriptor> Preferred, FDTTAutoNameResult& Out);

    /** What the dock holds right now: the most numerous item in it, or its fluid. Null when empty. */
    static TSubclassOf<UFGItemDescriptor> DockCargo(AFGBuildableDockingStation* Station);
};
