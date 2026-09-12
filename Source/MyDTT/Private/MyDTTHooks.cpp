#include "MyDTTHooks.h"

#include "Buildables/FGBuildableDockingStation.h"
#include "FGInventoryComponent.h"
#include "Resources/FGItemDescriptor.h"
#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"

#if !WITH_EDITOR
#include "Patching/NativeHookManager.h"
#include "WheeledVehicles/FGWheeledVehicle.h"
#include "FGDockableInterface.h"
#include "DynamicTimeTableSubsystem.h"
#include "Engine/World.h"
#endif

DEFINE_LOG_CATEGORY(LogMyDTTHooks);

namespace
{
    // The transfer runs inside the station's factory tick, which is not guaranteed to be the game
    // thread (the game defers the undock that follows it to a game-thread task). The plan table is
    // therefore the only thing the transfer hook reads besides the two inventories the original
    // touches, and it is locked.
    FCriticalSection GPlanLock;
    TMap<TWeakObjectPtr<const AFGBuildableDockingStation>, FDTTTransferPlan> GPlans;

    // Game thread only: written by the subsystem, read by ForceUndockActor/DockActor, which the
    // game itself asserts to be on the game thread.
    TSet<TWeakObjectPtr<const AFGBuildableDockingStation>> GHeld;

    bool GDockingHooksInstalled = false;
    bool GUndockHoldInstalled = false;

    void Installing(const TCHAR* Target)
    {
        // SML turns a funchook refusal into a fatal error inside StartupModule. Naming the target
        // first turns that from a stack trace into a log line saying which function was refused.
        UE_LOG(LogMyDTTHooks, Display, TEXT("installing: %s"), Target);
    }

    FString StationName(const AFGBuildableDockingStation* Station)
    {
        if (!IsValid(Station)) return TEXT("<none>");
        const AFGDockingStationIdentifier* Id = Station->GetStationIdentifier();
        return IsValid(Id) ? Id->GetStationName().ToString() : Station->GetName();
    }

    const TCHAR* ModeName(FDTTTransferPlan::EMode Mode)
    {
        switch (Mode)
        {
        case FDTTTransferPlan::EMode::Block: return TEXT("Block");
        case FDTTTransferPlan::EMode::FillTo: return TEXT("FillTo");
        default: return TEXT("PassThrough");
        }
    }
}

bool FMyDTTHooks::AreDockingHooksInstalled() { return GDockingHooksInstalled; }
bool FMyDTTHooks::IsUndockHoldInstalled() { return GUndockHoldInstalled; }

void FMyDTTHooks::SetTransferPlan(const AFGBuildableDockingStation* Station, const FDTTTransferPlan& Plan)
{
    if (!IsValid(Station)) return;
    FScopeLock Lock(&GPlanLock);
    GPlans.Add(Station, Plan);
}

void FMyDTTHooks::ClearTransferPlan(const AFGBuildableDockingStation* Station)
{
    if (!Station) return;
    FScopeLock Lock(&GPlanLock);
    GPlans.Remove(Station);
}

bool FMyDTTHooks::FindTransferPlan(const AFGBuildableDockingStation* Station, FDTTTransferPlan& OutPlan)
{
    FScopeLock Lock(&GPlanLock);
    if (const FDTTTransferPlan* Plan = GPlans.Find(Station))
    {
        OutPlan = *Plan;
        return true;
    }
    return false;
}

void FMyDTTHooks::SetHeld(AFGBuildableDockingStation* Station, bool bHeld, const TCHAR* Reason)
{
    if (!IsValid(Station)) return;
    const bool bWasHeld = GHeld.Contains(Station);
    if (bHeld) GHeld.Add(Station); else GHeld.Remove(Station);
    if (bWasHeld != bHeld)
    {
        UE_LOG(LogMyDTTHooks, Log, TEXT("HOLD Station='%s' Held=%s Reason=%s"),
            *StationName(Station), bHeld ? TEXT("true") : TEXT("false"), Reason);
    }
}

bool FMyDTTHooks::IsHeld(const AFGBuildableDockingStation* Station)
{
    return Station && GHeld.Contains(Station);
}

int32 FMyDTTHooks::CappedLoadUnload(const AFGBuildableDockingStation* Self, UFGInventoryComponent* Docked, const FDTTTransferPlan& Plan)
{
    UFGInventoryComponent* Station = IsValid(Self) ? Self->GetInventory() : nullptr;
    if (!IsValid(Station) || !IsValid(Docked)) return 0;

    const bool bLoad = Self->GetIsInLoadMode();
    UFGInventoryComponent* Src = bLoad ? Station : Docked;
    UFGInventoryComponent* Dst = bLoad ? Docked : Station;

    int32 Cap = 0;
    if (Plan.Mode == FDTTTransferPlan::EMode::FillTo && Plan.ItemClass)
    {
        Cap = FMath::Max(0, Plan.TargetItems - Dst->GetNumItems(Plan.ItemClass));
    }

    // The game moves fluids slot to slot and solids by AddStack; both with partial adds, and both
    // skip an item class once a stack of it failed to fit.
    const EResourceForm Form = Self->GetDockingStationResourceForm();
    const bool bFluid = Form == EResourceForm::RF_LIQUID || Form == EResourceForm::RF_GAS;

    int32 Moved = 0;
    TSet<TSubclassOf<UFGItemDescriptor>> Failed;
    const int32 SlotCount = bFluid ? FMath::Min(Src->GetSizeLinear(), Dst->GetSizeLinear()) : Src->GetSizeLinear();
    for (int32 Index = 0; Index < SlotCount && Moved < Cap; ++Index)
    {
        FInventoryStack Stack;
        if (!Src->GetStackFromIndex(Index, Stack) || !Stack.HasItems()) continue;
        const TSubclassOf<UFGItemDescriptor> Class = Stack.Item.GetItemClass();
        if (Failed.Contains(Class) || (Plan.ItemClass && Class != Plan.ItemClass)) continue;

        Stack.NumItems = FMath::Min(Stack.NumItems, Cap - Moved);
        const int32 Added = bFluid ? Dst->AddStackToIndex(Index, Stack, true, Src) : Dst->AddStack(Stack, true);
        if (Added <= 0)
        {
            Failed.Add(Class);
            continue;
        }
        Src->RemoveFromIndex(Index, Added, Dst);
        Moved += Added;
    }

    UE_LOG(LogMyDTTHooks, Log, TEXT("XFER_CAPPED Station='%s' Mode=%s Plan=%s Item=%s Target=%d Cap=%d Moved=%d"),
        *StationName(Self), bLoad ? TEXT("LOAD") : TEXT("UNLOAD"), ModeName(Plan.Mode),
        Plan.ItemClass ? *Plan.ItemClass->GetName() : TEXT("<any>"), Plan.TargetItems, Cap, Moved);
    return Moved;
}

void FMyDTTHooks::Install()
{
#if WITH_EDITOR
    UE_LOG(LogMyDTTHooks, Log, TEXT("Editor build: docking-station hooks disabled."));
#else
    // One call per load/unload cycle, at its end. A plan is present only while a truck this mod
    // manages stands on the station; every other docking runs the game's own transfer.
    Installing(TEXT("AFGBuildableDockingStation::Factory_LoadUnloadDockedInventory"));
    SUBSCRIBE_METHOD(AFGBuildableDockingStation::Factory_LoadUnloadDockedInventory,
        [](auto& Scope, const AFGBuildableDockingStation* Self, UFGInventoryComponent* Docked)
        {
            FDTTTransferPlan Plan;
            if (!FindTransferPlan(Self, Plan)) return;
            UE_LOG(LogMyDTTHooks, Log, TEXT("XFER_PROBE Station='%s' Plan=%s Thread=%s"),
                *StationName(Self), ModeName(Plan.Mode), IsInGameThread() ? TEXT("game") : TEXT("worker"));
            if (Plan.Mode == FDTTTransferPlan::EMode::PassThrough) return;
            Scope.Override(CappedLoadUnload(Self, Docked, Plan));
        });

    // The hold. The game undocks through this virtual, from a task deferred by
    // Factory_LoadUnloadVehicleComplete; cancelling it leaves the truck docked and the station
    // idle, since only DockActor starts a cycle. Virtual, so the sample object resolves the
    // implementation; no C++ subclass overrides it.
    Installing(TEXT("AFGBuildableDockingStation::ForceUndockActor"));
    SUBSCRIBE_METHOD_VIRTUAL(AFGBuildableDockingStation::ForceUndockActor, GetMutableDefault<AFGBuildableDockingStation>(),
        [](auto& Scope, AFGBuildableDockingStation* Self)
        {
            if (IsValid(Self) && Self->HasAuthority() && IsHeld(Self))
            {
                UE_LOG(LogMyDTTHooks, Log, TEXT("UNDOCK_HELD Station='%s' Docked='%s'"),
                    *StationName(Self), *GetNameSafe(Self->GetDockedActor()));
                Scope.Cancel();
            }
        });

    // Another actor docking evicts the held one through the same ForceUndockActor. Letting the
    // cancel win there would leave the station pointing at the newcomer while the parked truck
    // still believes it is docked; the hold is dropped first and the subsystem told.
    Installing(TEXT("AFGBuildableDockingStation::DockActor"));
    SUBSCRIBE_METHOD_VIRTUAL(AFGBuildableDockingStation::DockActor, GetMutableDefault<AFGBuildableDockingStation>(),
        [](auto& Scope, AFGBuildableDockingStation* Self, AActor* Actor)
        {
            if (!IsValid(Self) || !Self->HasAuthority() || !IsHeld(Self) || Actor == Self->GetDockedActor()) return;
            SetHeld(Self, false, TEXT("evicted by DockActor"));
            if (UWorld* World = Self->GetWorld())
                if (auto* S = World->GetSubsystem<UDynamicTimeTableSubsystem>())
                    S->HandleParkedTruckEvicted(Self, Actor);
        });
    GUndockHoldInstalled = true;

    // The docking events. Both are called by the station on the game thread (DockActor and
    // ForceUndockActor assert it), after the identifier's docking flag has been written.
    //
    // Hooked through the interface, not the vehicle class, on purpose. These overrides come from
    // IFGDockableInterface, a secondary base 0x648 bytes into AFGWheeledVehicle, and MSVC compiled
    // them with `this` = that interface subobject (the member pointer carries a this-adjustment of
    // 0x648; the function body reads mVehicleIdentifier at [rcx+0x180], impossible for an actor
    // pointer). SML hooks the resolved body and hands the handler the raw `this`, so a handler
    // typed AFGWheeledVehicle* got a pointer into the middle of the actor and IsValid() failed
    // silently: 2.0.0 as first built never saw a docking. Typed as the interface, `_getUObject()`
    // goes through the interface vtable and yields the actor.
    IFGDockableInterface* SampleDockable = GetMutableDefault<AFGWheeledVehicle>();
    Installing(TEXT("IFGDockableInterface::WasDocked_Implementation (AFGWheeledVehicle)"));
    SUBSCRIBE_METHOD_VIRTUAL_AFTER(IFGDockableInterface::WasDocked_Implementation, SampleDockable,
        [](IFGDockableInterface* Self, AActor* StationActor)
        {
            AFGWheeledVehicle* Vehicle = Self ? Cast<AFGWheeledVehicle>(Self->_getUObject()) : nullptr;
            if (!IsValid(Vehicle) || !Vehicle->HasAuthority()) return;
            if (UWorld* World = Vehicle->GetWorld())
                if (auto* S = World->GetSubsystem<UDynamicTimeTableSubsystem>())
                    S->HandleTruckDocked(Vehicle, Cast<AFGBuildableDockingStation>(StationActor));
        });
    Installing(TEXT("IFGDockableInterface::WasUndocked_Implementation (AFGWheeledVehicle)"));
    SUBSCRIBE_METHOD_VIRTUAL_AFTER(IFGDockableInterface::WasUndocked_Implementation, SampleDockable,
        [](IFGDockableInterface* Self)
        {
            AFGWheeledVehicle* Vehicle = Self ? Cast<AFGWheeledVehicle>(Self->_getUObject()) : nullptr;
            if (!IsValid(Vehicle) || !Vehicle->HasAuthority()) return;
            if (UWorld* World = Vehicle->GetWorld())
                if (auto* S = World->GetSubsystem<UDynamicTimeTableSubsystem>())
                    S->HandleTruckUndocked(Vehicle);
        });
    GDockingHooksInstalled = true;
    UE_LOG(LogMyDTTHooks, Display, TEXT("docking-station hooks installed"));
#endif
}
