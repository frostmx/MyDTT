#include "DynamicTimeTableModule.h"
#include "Modules/ModuleManager.h"
#if !WITH_EDITOR
#include "Patching/NativeHookManager.h"
#include "FGTrain.h"
#include "FGPlayerController.h"
#include "FGRemoteCallObject.h"
#include "FGRailroadTimeTable.h"
#include "FGTrainStationIdentifier.h"
#include "Buildables/FGBuildableRailroadStation.h"
#include "WheeledVehicles/FGDockingStationIdentifier.h"
#include "WheeledVehicles/FGWheeledVehicleIdentifier.h"
#include "DynamicTimeTableSubsystem.h"
#include "DynamicTimeTableRemoteCallObject.h"
#include "MyDTTHooks.h"
#include "Player/SMLRemoteCallObject.h"
#include "GameFramework/GameModeBase.h"
#include "Engine/World.h"
#include "TimerManager.h"
#endif
DEFINE_LOG_CATEGORY_STATIC(LogDynamicTimeTableModule,Log,All);
void FDynamicTimeTableModule::StartupModule(){UE_LOG(LogDynamicTimeTableModule,Log,TEXT("DynamicTimeTable module started."));
#if WITH_EDITOR
UE_LOG(LogDynamicTimeTableModule,Log,TEXT("Editor build: runtime registration disabled."));
#else
// The RCO is registered per player, and only for a player who actually has this mod.
// AFGPlayerController::mRemoteCallObjects is a replicated property, so the object is sent to the
// owning client as a subobject; a client that does not know the class cannot resolve it and drops
// the connection instead. That is the failure FicsitRemoteMonitoring #305 describes, and it is
// what makes "server-side only" more than a manifest flag.
GameModePostLoginHandle=FGameModeEvents::GameModePostLoginEvent.AddStatic(&FDynamicTimeTableModule::HandlePostLogin);
SUBSCRIBE_METHOD_AFTER(AFGTrain::OnDocked,[](AFGTrain* Train,AFGBuildableRailroadStation* Station){if(IsValid(Train)&&IsValid(Train->GetWorld()))if(auto* S=Train->GetWorld()->GetSubsystem<UDynamicTimeTableSubsystem>())S->HandleTrainDocked(Train,Station);});
SUBSCRIBE_METHOD_AFTER(AFGRailroadTimeTable::SetStops,[](const bool& Ok,AFGRailroadTimeTable* T,const TArray<FTimeTableStop>& Stops){(void)Stops;if(Ok&&IsValid(T)&&IsValid(T->GetWorld()))if(auto* S=T->GetWorld()->GetSubsystem<UDynamicTimeTableSubsystem>())S->HandleTimeTableChanged(T);});
SUBSCRIBE_METHOD_AFTER(AFGTrainStationIdentifier::SetStationName,[](AFGTrainStationIdentifier* Id,const FText& Name){(void)Name;if(IsValid(Id)&&IsValid(Id->GetWorld()))if(auto* S=Id->GetWorld()->GetSubsystem<UDynamicTimeTableSubsystem>())S->HandleStationNameChanged(Id);});
SUBSCRIBE_METHOD_AFTER(AFGTrain::SetSelfDrivingEnabled,[](AFGTrain* Train,bool Enabled){if(IsValid(Train)&&IsValid(Train->GetWorld()))if(auto* S=Train->GetWorld()->GetSubsystem<UDynamicTimeTableSubsystem>())S->HandleSelfDrivingChanged(Train,Enabled);});
// Truck counterparts. AFGWheeledVehicleIdentifier is to a truck what AFGRailroadTimeTable plus
// AFGTrain are to a train: it owns the route, the current target and the autopilot flag.
// Only the two BlueprintCallable setters are hooked. SetCurrentTargetWaypoint and
// SetIsCurrentlyDocking are plain native methods with no UFUNCTION, and hooks on them never
// fired in game - the subsystem polls for those two signals instead.
SUBSCRIBE_METHOD_AFTER(AFGWheeledVehicleIdentifier::SetVehicleRoute,[](AFGWheeledVehicleIdentifier* V,const TArray<FGuid>& Route){(void)Route;if(IsValid(V)&&IsValid(V->GetWorld()))if(auto* S=V->GetWorld()->GetSubsystem<UDynamicTimeTableSubsystem>())S->HandleVehicleRouteChanged(V);});
SUBSCRIBE_METHOD_AFTER(AFGWheeledVehicleIdentifier::SetAutopilotEnabled,[](AFGWheeledVehicleIdentifier* V,bool Enabled){if(IsValid(V)&&IsValid(V->GetWorld()))if(auto* S=V->GetWorld()->GetSubsystem<UDynamicTimeTableSubsystem>())S->HandleAutopilotChanged(V,Enabled);});
SUBSCRIBE_METHOD_AFTER(AFGDockingStationIdentifier::SetStationName,[](AFGDockingStationIdentifier* Id,const FText& Name){(void)Name;if(IsValid(Id)&&IsValid(Id->GetWorld()))if(auto* S=Id->GetWorld()->GetSubsystem<UDynamicTimeTableSubsystem>())S->HandleDockingStationNameChanged(Id);});
// Docking-station side: the transfer cap, the depot hold and the real docking events. See MyDTTHooks.
FMyDTTHooks::Install();
#endif
}
#if !WITH_EDITOR
void FDynamicTimeTableModule::HandlePostLogin(AGameModeBase* GameMode,APlayerController* Controller){TryRegisterRemoteCallObject(Cast<AFGPlayerController>(Controller),0);}

void FDynamicTimeTableModule::TryRegisterRemoteCallObject(AFGPlayerController* PC,int32 Attempt)
{
    if(!IsValid(PC))return;

    // The host always has the mod - it is running it. For anyone else, SML fills in the client's
    // mod list on its own RCO during PostLogin, so this can only be asked once that exists.
    bool bClientHasMod=PC->IsLocalController();
    if(!bClientHasMod)
    {
        USMLRemoteCallObject* SMLRCO=Cast<USMLRemoteCallObject>(PC->GetRemoteCallObjectOfClass(USMLRemoteCallObject::StaticClass()));
        if(!IsValid(SMLRCO))
        {
            // Delegate order is not a contract; if SML has not run yet, look again next tick.
            if(Attempt>=5||!IsValid(PC->GetWorld())){UE_LOG(LogDynamicTimeTableModule,Warning,TEXT("dTT RCO skipped: SML remote call object never appeared."));return;}
            TWeakObjectPtr<AFGPlayerController> WeakPC(PC);
            const int32 Next=Attempt+1;
            PC->GetWorld()->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateLambda([WeakPC,Next](){TryRegisterRemoteCallObject(WeakPC.Get(),Next);}));
            return;
        }
        bClientHasMod=SMLRCO->IsClientModInstalled(TEXT("MyDTT"));
    }

    if(!bClientHasMod){UE_LOG(LogDynamicTimeTableModule,Log,TEXT("dTT RCO not registered: client does not have the mod. The server keeps balancing without it."));return;}

    UFGRemoteCallObject* R=PC->RegisterRemoteCallObjectClass(UDynamicTimeTableRemoteCallObject::StaticClass());
    UE_LOG(LogDynamicTimeTableModule,Log,TEXT("dTT RCO registration. Success=%s"),IsValid(R)?TEXT("true"):TEXT("false"));
}
#endif

void FDynamicTimeTableModule::ShutdownModule(){
#if !WITH_EDITOR
if(GameModePostLoginHandle.IsValid()){FGameModeEvents::GameModePostLoginEvent.Remove(GameModePostLoginHandle);GameModePostLoginHandle.Reset();}
#endif
UE_LOG(LogDynamicTimeTableModule,Log,TEXT("DynamicTimeTable module shut down."));}
IMPLEMENT_MODULE(FDynamicTimeTableModule,MyDTT)
