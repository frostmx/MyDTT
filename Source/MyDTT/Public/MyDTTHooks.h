#pragma once

#include "CoreMinimal.h"
#include "Templates/SubclassOf.h"
#include "Resources/FGItemDescriptor.h"

/** Liquids and gases move slot to slot and count in 50 000 L stacks; everything else is a solid. */
inline bool DTSIsFluidForm(EResourceForm Form)
{
    return Form == EResourceForm::RF_LIQUID || Form == EResourceForm::RF_GAS;
}

class AFGBuildableDockingStation;
class UFGConnectionComponent;
class UFGInventoryComponent;
class UFGItemDescriptor;

DECLARE_LOG_CATEGORY_EXTERN(LogMyDTTHooks, Log, All);

/**
 * What a docking station may transfer to or from the truck standing on it. Set by the subsystem
 * on the game thread when the truck docks, applied by the transfer hook at the end of the
 * load/unload cycle (one call, measured), cleared when the truck undocks.
 *
 * FillTo is a target level of the destination inventory, not a quantity: the cap is
 * TargetItems minus what the destination already holds, evaluated when the transfer runs, so a
 * requester that filled up from a belt in the meantime is not overfilled.
 */
struct FDTTTransferPlan
{
    enum class EMode : uint8
    {
        PassThrough, // the game's own transfer runs
        Block,       // nothing moves (a depot, or an unplanned stop of a managed truck)
        FillTo,      // move ItemClass until the destination holds TargetItems
        Drain        // move ItemClass (every class when null) as far as the destination takes
    };

    EMode Mode = EMode::PassThrough;
    TSubclassOf<UFGItemDescriptor> ItemClass;
    int32 TargetItems = 0;
};

/**
 * The docking-station and vehicle hooks. A class rather than a namespace because the access
 * transformer makes it a friend of AFGBuildableDockingStation, which is what lets it name the
 * protected transfer method in SUBSCRIBE_METHOD.
 *
 * Proven by disassembly of the shipping FactoryGame module (12.09.2026):
 * - Factory_TickProducing calls Factory_LoadUnloadDockedInventory exactly once per cycle, when the
 *   cycle progress reaches 1.0, and then Factory_LoadUnloadVehicleComplete, which defers
 *   ForceUndockActor to a game-thread task. mIsLoadUnloading is raised only by DockActor, so a
 *   station whose undock was cancelled runs no further cycles.
 * - AFGWheeledVehicle::WasDocked_Implementation / WasUndocked_Implementation write the identifier's
 *   docking flag inline; they are the docking events (the identifier's setter is never called).
 * - DockActor undocks a previously docked actor through the same virtual ForceUndockActor.
 */
class MYDTT_API FMyDTTHooks
{
public:
    static void Install();

    /** WasDocked/WasUndocked reached the subsystem at least once since startup. */
    static bool AreDockingHooksInstalled();
    /** The ForceUndockActor and DockActor hooks installed without a refusal. */
    static bool IsUndockHoldInstalled();

    static void SetTransferPlan(const AFGBuildableDockingStation* Station, const FDTTTransferPlan& Plan);
    static void ClearTransferPlan(const AFGBuildableDockingStation* Station);

    /** A held station keeps its docked actor: the game's deferred undock is cancelled. */
    static void SetHeld(AFGBuildableDockingStation* Station, bool bHeld, const TCHAR* Reason);
    static bool IsHeld(const AFGBuildableDockingStation* Station);
    /**
     * Runs the transfer now, on the game thread, outside any docking cycle: the top-ups of a held
     * truck (a requester draining a leftover, a depot taking it). Quiet logs only when something
     * moved, since it runs once a second.
     */
    static int32 TransferNow(const AFGBuildableDockingStation* Station, UFGInventoryComponent* Docked, const FDTTTransferPlan& Plan, bool bQuiet);

    /**
     * The dock's cargo ports, connected ones only: belts and pipes into its storage in OutIns,
     * belts and pipes out of it in OutOuts. The fuel inputs are left out - a belt of coal into the
     * fuel slot says nothing about what the trucks carry. Lives here because the arrays that tell
     * the ports apart are protected, and this class is the station's friend.
     */
    static void GetDockPorts(const AFGBuildableDockingStation* Station,
        TArray<UFGConnectionComponent*>& OutIns, TArray<UFGConnectionComponent*>& OutOuts);

private:
    static bool FindTransferPlan(const AFGBuildableDockingStation* Station, FDTTTransferPlan& OutPlan);
    /** The game's transfer loop with a cap. Same inventory calls, same partial-add semantics. */
    static int32 CappedLoadUnload(const AFGBuildableDockingStation* Self, UFGInventoryComponent* Docked, const FDTTTransferPlan& Plan,
        const TCHAR* LogTag = TEXT("XFER_CAPPED"), bool bQuiet = false);
};
