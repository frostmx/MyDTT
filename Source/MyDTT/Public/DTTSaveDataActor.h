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
    const TArray<FDTTSavedTruckAnchors>& GetTruckAnchors() const { return SavedTruckAnchors; }
    const TArray<FDTTSavedPoolTruck>& GetPoolTrucks() const { return SavedPoolTrucks; }
    const FDTTSettings& GetSettings() const { return SavedSettings; }
    void SetSettings(const FDTTSettings& InSettings) { SavedSettings = InSettings; }
    bool IsPaintByCargo() const { return bSavedPaintByCargo; }
    void SetPaintByCargo(bool bInPaintByCargo) { bSavedPaintByCargo = bInPaintByCargo; }
    const TArray<FDTTSavedTruckPaint>& GetTruckPaints() const { return SavedTruckPaints; }
    const TArray<FDTTSavedDronePaint>& GetDronePaints() const { return SavedDronePaints; }

private:
    // 4 and later store no groups: the configuration lives in the station names themselves. The
    // actor stays because older saves reference the class, and an empty one is cheaper than the
    // risk of pulling a class out from under a load. 5 adds dispatch trucks' fuel anchors, 6 the
    // shared-pool trucks, 7 the settings, 8 the cargo paint, 9 the lap plans as parallel arrays
    // instead of a GUID-keyed map, 10 the drones' own paints.
    UPROPERTY(SaveGame)
    int32 DataVersion = 10;

    UPROPERTY(SaveGame)
    TArray<FDTTStationGroup> SavedGroups;

    // Refreshed from the subsystem right before every save. A lap that does not need a truck's own
    // (F) leaves it out of the route, so this is the only place that remembers it across a load.
    UPROPERTY(SaveGame)
    TArray<FDTTSavedTruckAnchors> SavedTruckAnchors;

    // Pool trucks, refreshed the same way. A truck saved while parked is held docked by this mod
    // alone; the game saves the docking itself, and this is what says the hold has to come back.
    UPROPERTY(SaveGame)
    TArray<FDTTSavedPoolTruck> SavedPoolTrucks;
    // The tunables, edited from the Settings tab. Read only when DataVersion is 7 or later.
    UPROPERTY(SaveGame)
    FDTTSettings SavedSettings;

    // Cargo paint (DataVersion 8). The trucks' current colours are saved by the game with the
    // vehicles; these two say whether the colours are this mod's and what was underneath.
    UPROPERTY(SaveGame)
    bool bSavedPaintByCargo = false;

    UPROPERTY(SaveGame)
    TArray<FDTTSavedTruckPaint> SavedTruckPaints;

    // DataVersion 10. Empty in an older save, which is also what "no drone was painted" looks like.
    UPROPERTY(SaveGame)
    TArray<FDTTSavedDronePaint> SavedDronePaints;
};
