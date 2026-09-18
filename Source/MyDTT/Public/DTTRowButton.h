#pragma once

#include "CoreMinimal.h"
#include "Components/Button.h"
#include "DTTRowButton.generated.h"

/**
 * What a control in a row of the window asks for. One enum for every page, because one handler
 * on the window carries them all out: a button has no business knowing what a park costs.
 */
UENUM()
enum class EDTTRowAction : uint8
{
    None,
    /** Open or close one card of the Dispatch page; the key says which. */
    ToggleExpandDispatch,
    /** The same for the Trucks page. Two values and not one, so no page has to be looked up. */
    ToggleExpandTruck,
    TruckPark,
    TruckRelease,
    TruckAutopilotOn,
    TruckAutopilotOff,
    TruckRebuildLap,
};

/**
 * What a click is about. Client-side only and never replicated: what crosses to the server is the
 * identifier actor out of this, which the game replicates to everyone by itself.
 *
 * The handles are weak on purpose. A row is drawn from a snapshot up to 1.5 s old, and a truck can
 * be sold or a station dismantled while its row is on the screen; the window checks the handle when
 * the click arrives rather than trusting the row it drew.
 */
struct FDTTRowTarget
{
    EDTTRowAction Action = EDTTRowAction::None;
    TWeakObjectPtr<class AFGWheeledVehicleIdentifier> Truck;
    TWeakObjectPtr<class AFGDockingStationIdentifier> Dock;
    /** Which card an expand toggles. Empty for everything else. */
    FString CardKey;
};

DECLARE_DELEGATE_OneParam(FDTTRowActionDelegate, const FDTTRowTarget&);

/**
 * A button that knows which row it is in.
 *
 * A subclass and not a map from widget to action, because OnClicked is a dynamic multicast with no
 * sender: there is nothing to look a loose button up by. And a subclass rather than one UFUNCTION
 * per row, because a UFUNCTION cannot be generated per row of live data.
 *
 * Bind once when the card is built, Attach on every pass over it. The widget is recycled by
 * position when a card is updated in place, so the row it stands in is not the row it stood in a
 * moment ago - a button that is only pointed at a truck on the build pass will eventually act on
 * somebody else's truck, and nothing in the log would say so.
 */
UCLASS(NotBlueprintable, NotPlaceable)
class MYDTT_API UDTTRowButton : public UButton
{
    GENERATED_BODY()

public:
    /** The window's one handler. Called once, on the pass that built this button. */
    void Bind(const FDTTRowActionDelegate& Delegate);
    /** Where this button points now. Called on every pass, build and update alike. */
    void Attach(const FDTTRowTarget& InTarget) { Target = InTarget; }
    const FDTTRowTarget& GetTarget() const { return Target; }

private:
    UFUNCTION()
    void HandleClicked();

    FDTTRowActionDelegate OnAction;
    FDTTRowTarget Target;
};
