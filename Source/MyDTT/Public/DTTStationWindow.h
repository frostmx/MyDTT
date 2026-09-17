#pragma once

#include "CoreMinimal.h"
#include "Components/Button.h"
#include "DTTStationWindow.generated.h"

/**
 * The button this mod adds to the game's own docking-station window, under the UNLOAD/LOAD switch:
 * it turns the "=" marker of the station's dispatch tag on and off, and reads its own state back
 * out of the station's name.
 *
 * A UButton subclass and not a plain one because a click has to land on a UFUNCTION of a UObject,
 * and because the button is the natural place to keep which station it belongs to: the game caches
 * its interact widgets, so the same button comes back in front of the next station.
 */
UCLASS(NotBlueprintable, NotPlaceable)
class MYDTT_API UDTTStationStayButton : public UButton
{
    GENERATED_BODY()

public:
    /** Points the button at the station whose window it is sitting in and draws its current state. */
    void Attach(class AFGBuildableDockingStation* InStation, class UTextBlock* InLabel);
    /** Re-reads the station's name; called from the watcher, so a rename shows up by itself. */
    void Refresh();

private:
    UFUNCTION()
    void HandleClicked();

    UPROPERTY()
    TWeakObjectPtr<class AFGBuildableDockingStation> Station;

    UPROPERTY()
    TObjectPtr<class UTextBlock> Label;

    /** What the name said at the last refresh; the click asks for the opposite. */
    bool bStayUntilEmpty = false;
    bool bHasDispatchTag = false;
};

/**
 * Client-side watcher. The game's station window is a Blueprint this mod does not own, so there is
 * nothing to subclass and nothing to bind: a timer looks at the interact-widget stack instead and
 * puts the button into whatever docking-station window is open. Nothing here runs on a dedicated
 * server, and a client without this mod simply has the window the game shipped.
 */
class FDTTStationWindow
{
public:
    /** One pass over the open interact widgets. Cheap: the stack holds a handful of widgets. */
    static void Tick(UWorld* World);
};
