#pragma once
#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class AGameModeBase;
class APlayerController;
class AFGPlayerController;

class FDynamicTimeTableModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
#if !WITH_EDITOR
    static void HandlePostLogin(AGameModeBase* GameMode, APlayerController* Controller);
    /** Registers the mod's RCO for this player, but only if that player has the mod installed. */
    static void TryRegisterRemoteCallObject(AFGPlayerController* PlayerController, int32 Attempt);
#endif
    FDelegateHandle GameModePostLoginHandle;
};
