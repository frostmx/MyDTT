#pragma once

#include "CoreMinimal.h"
#include "FGSaveInterface.h"
#include "GameFramework/Actor.h"
#include "DynamicTimeTableTypes.h"
#include "DTTSaveDataActor.generated.h"

UCLASS(NotBlueprintable, NotPlaceable)
class MYDTT_API ADTTSaveDataActor : public AActor, public IFGSaveInterface
{
    GENERATED_BODY()

public:
    ADTTSaveDataActor();

    virtual void BeginPlay() override;

    virtual void PreSaveGame_Implementation(int32 saveVersion, int32 gameVersion) override;
    virtual void PostSaveGame_Implementation(int32 saveVersion, int32 gameVersion) override;
    virtual void PreLoadGame_Implementation(int32 saveVersion, int32 gameVersion) override;
    virtual void PostLoadGame_Implementation(int32 saveVersion, int32 gameVersion) override;
    virtual void GatherDependencies_Implementation(TArray<UObject*>& out_dependentObjects) override;
    virtual bool NeedTransform_Implementation() override { return false; }
    virtual bool ShouldSave_Implementation() const override { return true; }

    void SetGroups(const TArray<FDTTStationGroup>& InGroups);
    const TArray<FDTTStationGroup>& GetGroups() const { return SavedGroups; }
    int32 GetDataVersion() const { return DataVersion; }
    void SetDataVersion(int32 InVersion) { DataVersion = InVersion; }

private:
    // 4 and later store nothing: the configuration lives in the station names themselves. The
    // actor stays because older saves reference the class, and an empty one is cheaper than the
    // risk of pulling a class out from under a load.
    UPROPERTY(SaveGame)
    int32 DataVersion = 4;

    UPROPERTY(SaveGame)
    TArray<FDTTStationGroup> SavedGroups;
};
