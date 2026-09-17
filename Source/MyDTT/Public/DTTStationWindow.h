#pragma once

#include "CoreMinimal.h"
#include "Components/VerticalBox.h"
#include "DTTStationWindow.generated.h"

/**
 * What this mod adds to the game's own docking-station window: a second switch under UNLOAD/LOAD
 * that turns the "=" marker of the station's dispatch tag on and off. The state is read back out
 * of the station's name, so there is nothing to keep in sync.
 *
 * The switch is the game's own BPW_OnOffSwitch, taken from the class of the UNLOAD/LOAD switch
 * standing right above it - no asset path to go stale, and the row looks like the row it sits
 * under. A window that does not give us one gets a plain button instead. It carries no caption of
 * its own: it is one more vehicle setting under the window's own "VEHICLE SETTINGS:", and a
 * station with no dispatch tag hides it rather than showing a switch that does nothing.
 *
 * A widget of its own and not a loose button because a switch reports through a Blueprint event
 * dispatcher, which needs a UFUNCTION to land on, and because the game caches its interact
 * widgets: the same control comes back in front of the next station and has to be re-pointed.
 */
UCLASS(NotBlueprintable, NotPlaceable)
class MYDTT_API UDTTStationStayControl : public UVerticalBox
{
    GENERATED_BODY()

public:
    /** Builds the switch once, from the class of the one it will sit under. */
    void Build(class UWidgetTree* Tree, UClass* SwitchClass);
    /**
     * Points the control at the station whose window it is in and draws its current state.
     * bWindowOpened says the window has just been put up, which is when the switch is written
     * whether or not it already agrees - see Refresh.
     */
    void Attach(class AFGBuildableDockingStation* InStation, bool bWindowOpened = false);
    /** Re-reads the station's name; called from the watcher, so a rename shows up by itself. */
    void Refresh();

private:
    UFUNCTION()
    void HandleSwitchChanged(bool bIsOn);

    UFUNCTION()
    void HandleButtonClicked();

    /** Sends the wanted state to whoever owns the world, then leaves the name to speak for itself. */
    void Request(bool bOn);

    UPROPERTY()
    TWeakObjectPtr<class AFGBuildableDockingStation> Station;

    /** The game's switch when the window lends us its class, otherwise null and Button is set. */
    UPROPERTY()
    TObjectPtr<class UUserWidget> Switch;

    UPROPERTY()
    TObjectPtr<class UButton> Button;

    UPROPERTY()
    TObjectPtr<class UTextBlock> ButtonLabel;

    /** What the name said at the last refresh; a flip of the switch asks for the opposite. */
    bool bStayUntilEmpty = false;
    bool bHasDispatchTag = false;
    /** What was last written into the switch, for a window whose switch cannot be read back. */
    bool bSwitchShows = false;
    /** Cleared whenever the window goes up, so the switch is told its state at least once. */
    bool bSwitchWritten = false;

    /** The name the two flags above were read out of; the same string is not parsed twice. */
    FString LastName;

    /** The window's own name field, kept in step with the station - see SyncNameField. */
    UPROPERTY()
    TWeakObjectPtr<class UWidget> NameField;
};

/**
 * Client-side watcher. The game's station window is a Blueprint this mod does not own, so there is
 * nothing to subclass and nothing to bind: a timer looks at the interact-widget stack instead and
 * puts the control into whatever docking-station window is open. Nothing here runs on a dedicated
 * server, and a client without this mod simply has the window the game shipped.
 */
class FDTTStationWindow
{
public:
    /** One pass over the open interact widgets. Cheap: the stack holds a handful of widgets. */
    static void Tick(UWorld* World);
};
