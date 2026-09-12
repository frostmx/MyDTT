#include "DTTSaveDataActor.h"
#include "DynamicTimeTableSubsystem.h"
#include "FGTrainStationIdentifier.h"
#include "WheeledVehicles/FGDockingStationIdentifier.h"
#include "WheeledVehicles/FGWheeledVehicleIdentifier.h"

DEFINE_LOG_CATEGORY_STATIC(LogDynamicTimeTableSaveData, Log, All);

ADTTSaveDataActor::ADTTSaveDataActor()
{
    PrimaryActorTick.bCanEverTick = false;
    SetActorHiddenInGame(true);
    SetCanBeDamaged(false);
    SetReplicates(false);
    bNetLoadOnClient = false;
}

void ADTTSaveDataActor::BeginPlay()
{
    Super::BeginPlay();
    UE_LOG(LogDynamicTimeTableSaveData, Display,
        TEXT("dTT save-data actor active. Version=%d Groups=%d TruckAnchors=%d PoolTrucks=%d"),
        DataVersion, SavedGroups.Num(), SavedTruckAnchors.Num(), SavedPoolTrucks.Num());
}

void ADTTSaveDataActor::SetGroups(const TArray<FDTTStationGroup>& InGroups)
{
    SavedGroups = InGroups;
}

void ADTTSaveDataActor::PreSaveGame_Implementation(int32 saveVersion, int32 gameVersion)
{
    // Pulled here rather than pushed on every change: anchors move every lap, and only the state at
    // the moment of saving matters.
    if (UWorld* World = GetWorld())
    {
        if (const UDynamicTimeTableSubsystem* Subsystem = World->GetSubsystem<UDynamicTimeTableSubsystem>())
        {
            Subsystem->ExportTruckAnchors(SavedTruckAnchors);
            Subsystem->ExportPoolTrucks(SavedPoolTrucks);
        }
    }
    UE_LOG(LogDynamicTimeTableSaveData, Display,
        TEXT("Saving dTT configuration. Groups=%d TruckAnchors=%d PoolTrucks=%d DataVersion=%d"),
        SavedGroups.Num(), SavedTruckAnchors.Num(), SavedPoolTrucks.Num(), DataVersion);
}

void ADTTSaveDataActor::PostSaveGame_Implementation(int32 saveVersion, int32 gameVersion)
{
}

void ADTTSaveDataActor::PreLoadGame_Implementation(int32 saveVersion, int32 gameVersion)
{
}

void ADTTSaveDataActor::PostLoadGame_Implementation(int32 saveVersion, int32 gameVersion)
{
    UE_LOG(LogDynamicTimeTableSaveData, Display,
        TEXT("Loaded dTT configuration. Groups=%d TruckAnchors=%d PoolTrucks=%d DataVersion=%d"),
        SavedGroups.Num(), SavedTruckAnchors.Num(), SavedPoolTrucks.Num(), DataVersion);
}

void ADTTSaveDataActor::GatherDependencies_Implementation(TArray<UObject*>& out_dependentObjects)
{
    for (const FDTTStationGroup& Group : SavedGroups)
    {
        for (const FDTTStationEntry& Entry : Group.Stations)
        {
            if (IsValid(Entry.StationIdentifier.Get()))
            {
                out_dependentObjects.AddUnique(Entry.StationIdentifier.Get());
            }
            if (IsValid(Entry.DockingStationIdentifier.Get()))
            {
                out_dependentObjects.AddUnique(Entry.DockingStationIdentifier.Get());
            }
        }
    }
    for (const FDTTSavedTruckAnchors& Saved : SavedTruckAnchors)
    {
        if (IsValid(Saved.Vehicle.Get()))
        {
            out_dependentObjects.AddUnique(Saved.Vehicle.Get());
        }
    }
    for (const FDTTSavedPoolTruck& Saved : SavedPoolTrucks)
    {
        if (IsValid(Saved.Vehicle.Get()))
        {
            out_dependentObjects.AddUnique(Saved.Vehicle.Get());
        }
    }
}
