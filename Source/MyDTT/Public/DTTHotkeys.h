#pragma once

#include "CoreMinimal.h"
#include "InputCoreTypes.h"
#include "Input/FGBoundMappingContextHandle.h"
#include "DTTHotkeys.generated.h"

class AFGPlayerController;
class UInputAction;
class UInputMappingContext;
struct FInputActionInstance;

/**
 * The keys that open the /dtt window: one for the window, one per tab, each the player's own.
 * They live in this client's GameUserSettings.ini, never in the save or on the server - a key
 * is a matter of the hand on the keyboard, not of the world - and are bound through the game's
 * own mapping-context wrapper on the player controller, so they sit beside the game's keys
 * rather than under them.
 */
UCLASS()
class MYDTT_API UDTTHotkeys : public UObject
{
    GENERATED_BODY()

public:
    /** The window's key; F1 when nothing has been chosen. */
    FKey WindowKey = EKeys::F1;
    /** One per EDTTTab, in that order; unassigned by default. */
    TArray<FKey> TabKeys;

    /** What the Settings tab offers: None, then F1..F12. */
    static const TArray<FKey>& Choices();
    static FString KeyLabel(const FKey& Key);

    void Load();
    void Save() const;
    /** Stores, saves and rebinds; a key that is not among Choices() is read as None. */
    void SetKeys(const FKey& Window, const TArray<FKey>& Tabs);

    /**
     * Binds to the local controller once it has its input component; called from the client's
     * timer until it succeeds, because on a remote client the controller gets its input after
     * the world has begun. Idempotent.
     */
    void EnsureInstalled(UWorld* World);
    bool IsInstalled() const { return Controller.IsValid() && BoundHandle.IsValid(); }

private:
    void Install(AFGPlayerController* PC);
    void Uninstall();
    void MapKeys();
    UFUNCTION() void HandleAction(const FInputActionInstance& Instance);

    UPROPERTY() TObjectPtr<UInputAction> WindowAction;
    UPROPERTY() TArray<TObjectPtr<UInputAction>> TabActions;
    UPROPERTY() TObjectPtr<UInputMappingContext> Context;
    TWeakObjectPtr<AFGPlayerController> Controller;
    FBoundMappingContextHandle BoundHandle;
    bool bActionsBound = false;
};
