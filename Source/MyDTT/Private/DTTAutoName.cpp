#include "DTTAutoName.h"

#include "FGConnectionComponent.h"
#include "FGFactoryConnectionComponent.h"
#include "FGInventoryComponent.h"
#include "FGPipeConnectionComponent.h"
#include "FGRecipe.h"
#include "MyDTTHooks.h"
#include "Buildables/FGBuildableConveyorAttachment.h"
#include "Buildables/FGBuildableConveyorBase.h"
#include "Buildables/FGBuildableConveyorMonitor.h"
#include "Buildables/FGBuildableDockingStation.h"
#include "Buildables/FGBuildableGeneratorFuel.h"
#include "Buildables/FGBuildableManufacturer.h"
#include "Buildables/FGBuildablePipeReservoir.h"
#include "Buildables/FGBuildablePipeline.h"
#include "Buildables/FGBuildablePipelineAttachment.h"
#include "Buildables/FGBuildableResourceExtractor.h"
#include "Buildables/FGBuildableSplitterSmart.h"
#include "Buildables/FGBuildableStorage.h"
#include "Buildables/FGBuildableTrainPlatformCargo.h"
#include "Resources/FGAnyUndefinedDescriptor.h"
#include "Resources/FGExtractableResourceInterface.h"
#include "Resources/FGItemDescriptor.h"
#include "Resources/FGNoneDescriptor.h"
#include "Resources/FGOverflowDescriptor.h"
#include "Resources/FGResourceDescriptor.h"
#include "Resources/FGWildCardDescriptor.h"

DEFINE_LOG_CATEGORY_STATIC(LogDTTAutoName, Log, All);

namespace
{
    using FDTSItem = TSubclassOf<UFGItemDescriptor>;
    using FDTSRates = TMap<FDTSItem, float>;

    /**
     * What a port is for, whichever kind it is. A belt connector says it outright; a pipe
     * connector says it only on a machine (consumer or producer), and says nothing on pipes,
     * junctions and pumps, where the fluid may run either way.
     */
    enum class EDTSPort : uint8 { Consumer, Producer, Any };

    bool DTSIsFluidItem(FDTSItem Item)
    {
        if (!Item) return false;
        return DTSIsFluidForm(UFGItemDescriptor::GetForm(Item));
    }

    /**
     * Fuel energy is stored per inventory unit, and a fluid's unit is a litre. Measured on the
     * class defaults on 16.09.2026: Desc_LiquidFuel holds 0.75 and Desc_LiquidTurboFuel 2.0 -
     * per litre, the 750 MJ/m3 of the wiki divided by a thousand - while Desc_Coal holds 300 per
     * lump. So the value is used as it is and only the rate is scaled to cubic metres, like every
     * other fluid figure here.
     */
    constexpr bool DTSFluidEnergyIsPerCubicMetre = false;

    /**
     * Only these carry goods through without consuming them. Everything else - another station, a
     * train platform, the sink, anything unknown - ends the branch. A container looks exactly like
     * a splitter from the outside, so this has to be a whitelist, not "has two connections".
     */
    bool DTSIsPassThrough(const AActor* Actor)
    {
        return Actor->IsA<AFGBuildableConveyorBase>() ||
               Actor->IsA<AFGBuildableConveyorAttachment>() ||
               Actor->IsA<AFGBuildableConveyorMonitor>() ||
               Actor->IsA<AFGBuildablePipeline>() ||
               Actor->IsA<AFGBuildablePipelineAttachment>();   // junctions, pumps, valves
    }

    /**
     * A container between a station and its factory is a buffer, not a consumer: measured in the
     * live world, most docks feed or are fed through one, and stopping there answered "no machines"
     * for almost every station. It stays a terminator for the final-product hunt, where passing
     * through it would wander into the next factory over. A fluid buffer is the same thing for pipes.
     */
    bool DTSIsBuffer(const AActor* Actor)
    {
        return Actor->IsA<AFGBuildableStorage>() || Actor->IsA<AFGBuildablePipeReservoir>();
    }

    /** What the walk counts: things that eat or make items. A generator eats. */
    bool DTSIsMachine(const AActor* Actor)
    {
        return Actor->IsA<AFGBuildableManufacturer>() ||
               Actor->IsA<AFGBuildableResourceExtractor>() ||
               Actor->IsA<AFGBuildableGeneratorFuel>();
    }

    EDTSPort DTSPortKind(const UFGConnectionComponent* Port)
    {
        if (const UFGFactoryConnectionComponent* Belt = Cast<UFGFactoryConnectionComponent>(Port))
        {
            switch (Belt->GetDirection())
            {
            case EFactoryConnectionDirection::FCD_INPUT: return EDTSPort::Consumer;
            case EFactoryConnectionDirection::FCD_OUTPUT: return EDTSPort::Producer;
            default: return EDTSPort::Any;
            }
        }
        if (const UFGPipeConnectionComponent* Pipe = Cast<UFGPipeConnectionComponent>(Port))
        {
            switch (Pipe->GetPipeConnectionType())
            {
            case EPipeConnectionType::PCT_CONSUMER: return EDTSPort::Consumer;
            case EPipeConnectionType::PCT_PRODUCER: return EDTSPort::Producer;
            default: return EDTSPort::Any;
            }
        }
        return EDTSPort::Any;
    }

    bool DTSIsConnected(const UFGConnectionComponent* Port)
    {
        if (const UFGFactoryConnectionComponent* Belt = Cast<UFGFactoryConnectionComponent>(Port)) return Belt->IsConnected();
        if (const UFGPipeConnectionComponentBase* Pipe = Cast<UFGPipeConnectionComponentBase>(Port)) return Pipe->IsConnected();
        return false;
    }

    /** The port on the building at the other end of this port's belt or pipe link. */
    UFGConnectionComponent* DTSFarEnd(const UFGConnectionComponent* Port)
    {
        if (const UFGFactoryConnectionComponent* Belt = Cast<UFGFactoryConnectionComponent>(Port)) return Belt->GetConnection();
        if (const UFGPipeConnectionComponentBase* Pipe = Cast<UFGPipeConnectionComponentBase>(Port)) return Pipe->GetConnection();
        return nullptr;
    }

    void DTSCollectPorts(AActor* Actor, TArray<UFGConnectionComponent*>& Out)
    {
        Out.Reset();
        if (!IsValid(Actor)) return;
        TArray<UFGFactoryConnectionComponent*> Belts;
        Actor->GetComponents<UFGFactoryConnectionComponent>(Belts);
        for (UFGFactoryConnectionComponent* Belt : Belts) Out.Add(Belt);
        TArray<UFGPipeConnectionComponent*> Pipes;
        Actor->GetComponents<UFGPipeConnectionComponent>(Pipes);
        for (UFGPipeConnectionComponent* Pipe : Pipes) Out.Add(Pipe);
    }

    /** A belt or a pipe has exactly two ends; this is the one we did not come in by. */
    UFGConnectionComponent* DTSOtherEnd(AActor* Node, const UFGConnectionComponent* Entered)
    {
        if (AFGBuildableConveyorBase* Belt = Cast<AFGBuildableConveyorBase>(Node))
        {
            UFGFactoryConnectionComponent* Zero = Belt->GetConnection0();
            return Entered == Zero ? Belt->GetConnection1() : Zero;
        }
        if (AFGBuildablePipeline* Pipe = Cast<AFGBuildablePipeline>(Node))
        {
            UFGPipeConnectionComponent* Zero = Pipe->GetPipeConnection0();
            return Entered == Zero ? Pipe->GetPipeConnection1() : Zero;
        }
        return nullptr;
    }

    /**
     * Which output of a smart splitter this connection is. The rules index
     * AFGBuildableConveyorAttachment::mOutputs, which is protected - reading it would cost an access
     * transformer and a full FactoryGame rebuild. The components are named "...Output1".."3", and
     * EfficiencyCheckerMod has been reading the index off that name for years.
     */
    int32 DTSOutputIndexFromName(const UFGFactoryConnectionComponent* Conn)
    {
        const FString Name = Conn->GetName();
        if (Name.IsEmpty()) return INDEX_NONE;
        const TCHAR Last = Name[Name.Len() - 1];
        return FChar::IsDigit(Last) ? static_cast<int32>(Last - TEXT('1')) : INDEX_NONE;
    }

    /** Can Item leave this splitter through this output? Plain splitters and mergers always say yes. */
    enum class EDTSRule : uint8 { Blocked, Overflow, Allowed };

    /**
     * What a smart splitter's output does with Item: passes it, blocks it, or takes it only as
     * overflow - whatever the other outputs would not accept. The walks treat overflow as passable,
     * a deliberate over-count of demand; the share model does not discount an overflow branch,
     * because it takes nothing from a line whose other branches keep up.
     */
    EDTSRule DTSSplitterRule(AActor* Attachment, const UFGFactoryConnectionComponent* Out, FDTSItem Item)
    {
        const AFGBuildableSplitterSmart* Smart = Cast<AFGBuildableSplitterSmart>(Attachment);
        if (!Smart || !Item) return EDTSRule::Allowed;

        const TArray<FSplitterSortRule> Rules = Smart->GetSortRules();
        if (Rules.Num() == 0) return EDTSRule::Allowed;

        const int32 Index = DTSOutputIndexFromName(Out);
        if (Index == INDEX_NONE) return EDTSRule::Allowed;

        bool bHasOwnRule = false, bWild = false, bOverflow = false, bAnyUndefined = false, bNone = false;
        bool bExactHere = false, bNamedElsewhere = false;
        for (const FSplitterSortRule& Rule : Rules)
        {
            UClass* RuleClass = Rule.ItemClass.Get();
            if (!RuleClass) continue;
            const bool bMine = Rule.OutputIndex == Index;
            const bool bWildClass = RuleClass->IsChildOf(UFGWildCardDescriptor::StaticClass());
            const bool bOverflowClass = RuleClass->IsChildOf(UFGOverflowDescriptor::StaticClass());
            const bool bUndefClass = RuleClass->IsChildOf(UFGAnyUndefinedDescriptor::StaticClass());
            const bool bNoneClass = RuleClass->IsChildOf(UFGNoneDescriptor::StaticClass());

            if (!bWildClass && !bOverflowClass && !bUndefClass && !bNoneClass && RuleClass == Item.Get())
            {
                if (bMine) bExactHere = true; else bNamedElsewhere = true;
            }
            if (bMine)
            {
                bHasOwnRule = true;
                bWild |= bWildClass;
                bOverflow |= bOverflowClass;
                bAnyUndefined |= bUndefClass;
                bNone |= bNoneClass;
            }
        }

        if (bExactHere || bWild) return EDTSRule::Allowed;
        if (bOverflow) return EDTSRule::Overflow;
        if (bAnyUndefined) return bNamedElsewhere ? EDTSRule::Blocked : EDTSRule::Allowed;
        if (bNone) return EDTSRule::Blocked;
        return bHasOwnRule ? EDTSRule::Blocked : EDTSRule::Allowed;   // an output nobody filtered carries everything
    }

    bool DTSSplitterAllows(AActor* Attachment, const UFGFactoryConnectionComponent* Out, FDTSItem Item)
    {
        return DTSSplitterRule(Attachment, Out, Item) != EDTSRule::Blocked;
    }

    /** Megajoules in one inventory unit of this fuel: an item, or a litre of a fluid. */
    float DTSEnergyPerUnit(FDTSItem Fuel)
    {
        float Energy = UFGItemDescriptor::GetEnergyValue(Fuel);
        if (DTSIsFluidItem(Fuel) && DTSFluidEnergyIsPerCubicMetre) Energy /= 1000.0f;
        return Energy;
    }

    /**
     * Everything this generator burns, per minute, by item. Capacity times sixty over the energy of
     * one unit: a 75 MW coal generator on 300 MJ coal is 15 a minute. A generator that has never
     * been fed names no fuel yet, so every fuel it accepts goes in as a candidate and the name
     * carries a '?' until the first delivery settles it.
     */
    void DTSAddGeneratorDemand(AFGBuildableGeneratorFuel* Generator, FDTSRates& Out)
    {
        const float MW = Generator->CalcPowerProductionCapacityForPotential(Generator->GetPendingPotential());
        if (MW <= KINDA_SMALL_NUMBER) return;

        TArray<FDTSItem> Fuels;
        if (const FDTSItem Current = Generator->GetCurrentFuelClass()) Fuels.Add(Current);
        else
        {
            for (const TSoftClassPtr<UFGItemDescriptor>& Soft : Generator->GetDefaultFuelClasses())
            {
                if (UClass* Loaded = Soft.Get()) Fuels.Add(Loaded);
            }
        }
        for (const FDTSItem& Fuel : Fuels)
        {
            const float Energy = DTSEnergyPerUnit(Fuel);
            if (Energy <= KINDA_SMALL_NUMBER) continue;
            float Rate = MW * 60.0f / Energy;
            if (DTSIsFluidItem(Fuel)) Rate /= 1000.0f;
            Out.FindOrAdd(Fuel) += Rate;
        }

        // Water for a coal or nuclear plant. It arrives by pipe from pumps and is never trucked,
        // but it has to be in the count so that the supply check can see it covered.
        if (Generator->GetRequiresSupplementalResource())
        {
            if (const FDTSItem Water = Generator->GetSupplementalResourceClass())
            {
                const float Rate = Generator->GetSupplementalConsumptionRateMaximum() * 60.0f;
                if (Rate > KINDA_SMALL_NUMBER) Out.FindOrAdd(Water) += Rate;
            }
        }
    }

    /**
     * Everything this machine eats, per minute, by item.
     *
     * Deliberately not asked per port. The plan assumed an input connection's
     * GetInventoryAccessIndex() names the ingredient that arrives on that belt; measured in the live
     * world on 16.09.2026 it does not. A circuit-board assembler had the copper-sheet belt on the
     * connector bound to the plastic slot and ran perfectly - the game sorts an arriving item into
     * whichever slot accepts it, no matter which connector it came in by. So a machine is read as
     * wanting all of its ingredients, and which one a station actually carries is decided by the
     * dock's own cargo.
     */
    void DTSAddDemand(AActor* Machine, FDTSRates& Out)
    {
        if (AFGBuildableGeneratorFuel* Generator = Cast<AFGBuildableGeneratorFuel>(Machine))
        {
            DTSAddGeneratorDemand(Generator, Out);
            return;
        }

        AFGBuildableManufacturer* Manufacturer = Cast<AFGBuildableManufacturer>(Machine);
        if (!IsValid(Manufacturer)) return;

        const TSubclassOf<UFGRecipe> Recipe = Manufacturer->GetCurrentRecipe();
        if (!Recipe) return;

        const float Cycle = Manufacturer->GetDefaultProductionCycleTime();
        if (Cycle <= KINDA_SMALL_NUMBER) return;

        for (const FItemAmount& Ingredient : UFGRecipe::GetIngredients(Manufacturer, Recipe))
        {
            if (!Ingredient.ItemClass || Ingredient.Amount <= 0) continue;
            // The production boost multiplies the output, not the appetite, so it is absent here
            // and present in DTSAddSupply.
            float Rate = Ingredient.Amount * Manufacturer->GetPendingPotential() * 60.0f / Cycle;
            if (DTSIsFluidItem(Ingredient.ItemClass)) Rate /= 1000.0f;
            Out.FindOrAdd(Ingredient.ItemClass) += Rate;
        }
    }

    /** Everything this machine makes, per minute, by item. A generator makes power, not items. */
    void DTSAddSupply(AActor* Machine, FDTSRates& Out)
    {
        if (AFGBuildableManufacturer* Manufacturer = Cast<AFGBuildableManufacturer>(Machine))
        {
            const TSubclassOf<UFGRecipe> Recipe = Manufacturer->GetCurrentRecipe();
            if (!Recipe) return;
            const float Cycle = Manufacturer->GetDefaultProductionCycleTime();
            if (Cycle <= KINDA_SMALL_NUMBER) return;

            for (const FItemAmount& Product : UFGRecipe::GetProducts(Recipe))
            {
                if (!Product.ItemClass || Product.Amount <= 0) continue;
                float Rate = Product.Amount * Manufacturer->GetPendingPotential() *
                    Manufacturer->GetPendingProductionBoost() * 60.0f / Cycle;
                if (DTSIsFluidItem(Product.ItemClass)) Rate /= 1000.0f;
                Out.FindOrAdd(Product.ItemClass) += Rate;
            }
            return;
        }

        if (AFGBuildableResourceExtractor* Extractor = Cast<AFGBuildableResourceExtractor>(Machine))
        {
            const TScriptInterface<IFGExtractableResourceInterface> Resource = Extractor->GetExtractableResource();
            IFGExtractableResourceInterface* Extractable = Resource.GetInterface();
            if (!Extractable) return;

            const TSubclassOf<UFGResourceDescriptor> ResourceClass = Extractable->GetResourceClass();
            if (!ResourceClass) return;

            const float Cycle = Extractor->GetDefaultExtractCycleTime();
            const float Purity = Extractable->GetExtractionSpeedMultiplier();
            if (Cycle <= KINDA_SMALL_NUMBER || Purity <= KINDA_SMALL_NUMBER) return;

            float Rate = Extractor->GetNumExtractedItemsPerCycle() * Extractor->GetPendingPotential() *
                Extractor->GetPendingProductionBoost() * 60.0f / (Purity * Cycle);

            FDTSItem Item = ResourceClass;
            if (DTSIsFluidItem(Item)) Rate /= 1000.0f;
            Out.FindOrAdd(Item) += Rate;
        }
    }

    struct FDTSWalk
    {
        int32 NodeBudget = 500;
        bool bOverflow = false;
    };

    /** How one walk moves through the network. */
    struct FDTSWalkSpec
    {
        /** The ports a pass-through node is left by: outputs going downstream, inputs going up. */
        EDTSPort Forward = EDTSPort::Producer;
        /** The port a machine has to be reached by to count: its input downstream, its output up. */
        EDTSPort MachinePort = EDTSPort::Consumer;
        /** Smart-splitter filter, belts only. Null on the first pass, when the item is unknown. */
        FDTSItem Filter = nullptr;
        bool bThroughBuffers = true;
        /** Shared between seeds when given: one chain, one budget, one set of visited buildings. */
        TSet<AActor*>* Visited = nullptr;
    };

    /**
     * The one walk. Belts carry a direction on every connector; pipes carry one only on machines,
     * so a pipe network is explored in every direction and sorted out at the machines it reaches:
     * a demand walk that runs into a refinery's output has found a producer on the same pipe, not a
     * consumer, and leaves it to the supply check. Machines are handed to OnMachine with the port
     * they were reached by; buffers passed and train platforms hit are collected in OutStores.
     */
    void DTSWalk(const TArray<UFGConnectionComponent*>& Seeds, const FDTSWalkSpec& Spec, FDTSWalk& Walk,
        TFunctionRef<void(AActor*, UFGConnectionComponent*)> OnMachine, TSet<AActor*>* OutStores)
    {
        TSet<AActor*> LocalVisited;
        TSet<AActor*>& Visited = Spec.Visited ? *Spec.Visited : LocalVisited;
        TArray<UFGConnectionComponent*> Frontier = Seeds;

        while (Frontier.Num() > 0)
        {
            UFGConnectionComponent* In = Frontier.Pop(EAllowShrinking::No);
            if (!IsValid(In)) continue;

            AActor* Node = In->GetOwner();
            if (!IsValid(Node) || Visited.Contains(Node)) continue;
            Visited.Add(Node);

            if (--Walk.NodeBudget <= 0) { Walk.bOverflow = true; return; }

            if (DTSIsMachine(Node))
            {
                // An untyped port on a machine would be a blueprint this code has not met; it is
                // taken rather than dropped, and logged so the measurement shows it.
                const EDTSPort Kind = DTSPortKind(In);
                if (Kind == EDTSPort::Any)
                {
                    UE_LOG(LogDTTAutoName, Verbose, TEXT("AUTONAME_UNTYPED_PORT Machine=%s Port=%s"), *Node->GetName(), *In->GetName());
                }
                if (Kind == Spec.MachinePort || Kind == EDTSPort::Any) OnMachine(Node, In);
                continue;
            }

            if (Node->IsA<AFGBuildableTrainPlatformCargo>())
            {
                if (OutStores) OutStores->Add(Node);
                continue;
            }

            const bool bBuffer = Spec.bThroughBuffers && DTSIsBuffer(Node);
            if (bBuffer && OutStores) OutStores->Add(Node);
            if (!bBuffer && !DTSIsPassThrough(Node)) continue;

            if (Node->IsA<AFGBuildableConveyorBase>() || Node->IsA<AFGBuildablePipeline>())
            {
                UFGConnectionComponent* Far = DTSOtherEnd(Node, In);
                if (IsValid(Far) && DTSIsConnected(Far)) Frontier.Add(DTSFarEnd(Far));
                continue;
            }

            TArray<UFGConnectionComponent*> Ports;
            DTSCollectPorts(Node, Ports);
            for (UFGConnectionComponent* Port : Ports)
            {
                if (Port == In || !IsValid(Port) || !DTSIsConnected(Port)) continue;
                const EDTSPort Kind = DTSPortKind(Port);
                if (Kind != Spec.Forward && Kind != EDTSPort::Any) continue;
                if (!bBuffer && Spec.Filter)
                {
                    const UFGFactoryConnectionComponent* Belt = Cast<UFGFactoryConnectionComponent>(Port);
                    if (Belt && !DTSSplitterAllows(Node, Belt, Spec.Filter)) continue;
                }
                Frontier.Add(DTSFarEnd(Port));
            }
        }
    }

    /** The far ends of these ports, as seeds for a walk. */
    TArray<UFGConnectionComponent*> DTSSeeds(const TArray<UFGConnectionComponent*>& Ports)
    {
        TArray<UFGConnectionComponent*> Seeds;
        for (UFGConnectionComponent* Port : Ports)
        {
            if (IsValid(Port) && DTSIsConnected(Port)) Seeds.Add(DTSFarEnd(Port));
        }
        return Seeds;
    }

    /** The machine's own ports of one kind, connected ones only. */
    TArray<UFGConnectionComponent*> DTSMachinePorts(AActor* Machine, EDTSPort Kind)
    {
        TArray<UFGConnectionComponent*> Ports, Out;
        DTSCollectPorts(Machine, Ports);
        for (UFGConnectionComponent* Port : Ports)
        {
            if (IsValid(Port) && DTSIsConnected(Port) && DTSPortKind(Port) == Kind) Out.Add(Port);
        }
        return Out;
    }

    /**
     * The ingredient bound to the slot this input connection reaches. It does not say what travels
     * on the belt - the game sorts an arriving item into whichever slot accepts it - but on a dock
     * that has never been delivered to, nothing else says anything at all, and a builder who wired
     * each ingredient to its own port is answered exactly right.
     */
    FDTSItem DTSPortIngredient(AFGBuildableManufacturer* Manufacturer, const UFGFactoryConnectionComponent* In)
    {
        if (!IsValid(Manufacturer) || !IsValid(In)) return nullptr;
        const TSubclassOf<UFGRecipe> Recipe = Manufacturer->GetCurrentRecipe();
        if (!Recipe) return nullptr;

        const int32 Index = In->GetInventoryAccessIndex();
        if (Index < 0) return nullptr;

        const TArray<FItemAmount> Ingredients = UFGRecipe::GetIngredients(Manufacturer, Recipe);
        if (!Ingredients.IsValidIndex(Index)) return nullptr;
        return Ingredients[Index].ItemClass;
    }

    /**
     * What already arrives at these machines from something that makes it, by belt or by pipe.
     *
     * The strongest thing that can be said about an empty dock: whatever the chain needs and
     * nobody else delivers is what this dock is for. A truck station upstream ends the walk
     * silently and supplies nothing, so two docks feeding one assembler stay ambiguous - which is
     * honest, that case really is a coin flip.
     */
    void DTSCollectSupply(const TSet<AActor*>& Machines, FDTSWalk& Walk, FDTSRates& Out)
    {
        // One walk for the whole chain, not one per machine: twenty foundries share the belts
        // behind them, and walking each separately spends the node budget on the same conveyors
        // twenty times over - which made the answer "I cannot tell" on exactly the big factories
        // this is for.
        TSet<AActor*> Visited;
        TArray<UFGConnectionComponent*> Seeds;
        for (AActor* Machine : Machines)
        {
            Visited.Add(Machine);
            Seeds.Append(DTSSeeds(DTSMachinePorts(Machine, EDTSPort::Consumer)));
        }

        FDTSWalkSpec Spec;
        Spec.Forward = EDTSPort::Consumer;
        Spec.MachinePort = EDTSPort::Producer;
        Spec.Visited = &Visited;
        DTSWalk(Seeds, Spec, Walk, [&Out](AActor* Machine, UFGConnectionComponent*)
        {
            FDTSRates Made;
            DTSAddSupply(Machine, Made);
            for (const TPair<FDTSItem, float>& Pair : Made) Out.FindOrAdd(Pair.Key) += Pair.Value;
        }, nullptr);
    }

    enum class EDTSFlow : uint8 { Supply, Demand };

    /**
     * Items of Item per minute crossing Entry - the port a walk arrives at - in the direction
     * asked: Supply is what comes out of that port towards the caller, Demand is what the branch
     * behind it takes. EfficiencyCheckerMod's model: at a splitter the other outputs' demand comes
     * off the supply, at a merger the other inputs' supply comes off the demand, so a dock at the
     * end of a shared line shows the share that reaches it and not the whole line. A container
     * passes flow through and takes nothing; another truck station, a train platform and the sink
     * end the branch and take nothing - what a truck carries away is not a rate of this factory.
     * A pipe junction's ports have no direction, so they all count on the near side and nothing is
     * discounted: a fluid network is reported whole.
     */
    float DTSFlow(UFGConnectionComponent* Entry, FDTSItem Item, EDTSFlow Mode, FDTSWalk& Walk, TSet<AActor*>& Visited, int32 Depth)
    {
        if (!IsValid(Entry) || Depth > 64 || Walk.bOverflow) return 0.0f;
        AActor* Node = Entry->GetOwner();
        if (!IsValid(Node) || Visited.Contains(Node)) return 0.0f;
        Visited.Add(Node);
        if (--Walk.NodeBudget <= 0) { Walk.bOverflow = true; return 0.0f; }

        const EDTSPort Kind = DTSPortKind(Entry);
        if (DTSIsMachine(Node))
        {
            FDTSRates Rates;
            if (Mode == EDTSFlow::Supply && Kind != EDTSPort::Consumer) DTSAddSupply(Node, Rates);
            if (Mode == EDTSFlow::Demand && Kind != EDTSPort::Producer) DTSAddDemand(Node, Rates);
            const float* Found = Rates.Find(Item);
            return Found ? *Found : 0.0f;
        }

        if (Node->IsA<AFGBuildableConveyorBase>() || Node->IsA<AFGBuildablePipeline>())
        {
            UFGConnectionComponent* Far = DTSOtherEnd(Node, Entry);
            if (!IsValid(Far) || !DTSIsConnected(Far)) return 0.0f;
            return DTSFlow(DTSFarEnd(Far), Item, Mode, Walk, Visited, Depth + 1);
        }

        if (!DTSIsPassThrough(Node) && !DTSIsBuffer(Node)) return 0.0f;

        // Ports on the near side carry the flow asked for; ports on the far side take some of it.
        const EDTSPort Near = Mode == EDTSFlow::Supply ? EDTSPort::Consumer : EDTSPort::Producer;
        const EDTSFlow Opposite = Mode == EDTSFlow::Supply ? EDTSFlow::Demand : EDTSFlow::Supply;
        float Along = 0.0f;
        float Taken = 0.0f;
        TArray<UFGConnectionComponent*> Ports;
        DTSCollectPorts(Node, Ports);
        for (UFGConnectionComponent* Port : Ports)
        {
            if (Port == Entry || !IsValid(Port) || !DTSIsConnected(Port)) continue;
            const EDTSPort PortKind = DTSPortKind(Port);
            const UFGFactoryConnectionComponent* Belt = Cast<UFGFactoryConnectionComponent>(Port);
            const EDTSRule Rule = Belt && PortKind == EDTSPort::Producer ? DTSSplitterRule(Node, Belt, Item) : EDTSRule::Allowed;
            if (PortKind == Near || PortKind == EDTSPort::Any)
            {
                if (Rule == EDTSRule::Blocked) continue;
                Along += DTSFlow(DTSFarEnd(Port), Item, Mode, Walk, Visited, Depth + 1);
            }
            else
            {
                // A splitter's other output in a supply walk, a merger's other input in a demand
                // walk. An overflow output takes only what is left, so it takes nothing here.
                if (Rule != EDTSRule::Allowed) continue;
                Taken += DTSFlow(DTSFarEnd(Port), Item, Opposite, Walk, Visited, Depth + 1);
            }
        }
        return FMath::Max(0.0f, Along - Taken);
    }

    /**
     * From a machine forward to whatever the chain finally makes. A branch that reaches another
     * manufacturer keeps going; a branch that dies at a container, a station or a bare belt end
     * makes the last manufacturer on it the final one. A generator is final by nature and adds its
     * capacity instead of an item.
     */
    void DTSCollectFinalProducts(AActor* Machine, FDTSWalk& Walk, FDTSRates& Out, float& OutPowerMW,
        TSet<AActor*>& VisitedMachines, int32 Depth)
    {
        if (Depth > 16 || !IsValid(Machine) || VisitedMachines.Contains(Machine)) return;
        VisitedMachines.Add(Machine);

        if (AFGBuildableGeneratorFuel* Generator = Cast<AFGBuildableGeneratorFuel>(Machine))
        {
            OutPowerMW += Generator->CalcPowerProductionCapacityForPotential(Generator->GetPendingPotential());
            return;
        }

        AFGBuildableManufacturer* Manufacturer = Cast<AFGBuildableManufacturer>(Machine);
        if (!Manufacturer) return;

        FDTSRates OwnProducts;
        DTSAddSupply(Manufacturer, OwnProducts);

        bool bFedAnother = false;
        FDTSWalkSpec Spec;
        Spec.bThroughBuffers = false;
        for (UFGConnectionComponent* Port : DTSMachinePorts(Machine, EDTSPort::Producer))
        {
            TSet<AActor*> Downstream;
            DTSWalk({ DTSFarEnd(Port) }, Spec, Walk, [&Downstream](AActor* Next, UFGConnectionComponent*) { Downstream.Add(Next); }, nullptr);
            // A walk cut short by the budget has not seen where this port leads; calling the
            // machine final on that evidence would print an intermediate as a product.
            if (Walk.bOverflow) return;
            for (AActor* Next : Downstream)
            {
                bFedAnother = true;
                DTSCollectFinalProducts(Next, Walk, Out, OutPowerMW, VisitedMachines, Depth + 1);
                if (Walk.bOverflow) return;
            }
        }

        if (!bFedAnother)
        {
            for (const TPair<FDTSItem, float>& Pair : OwnProducts) Out.FindOrAdd(Pair.Key) += Pair.Value;
        }
    }

    /** The most numerous item in an inventory, or null. */
    void DTSTallyInventory(UFGInventoryComponent* Inventory, TMap<FDTSItem, int64>& Out)
    {
        if (!IsValid(Inventory)) return;
        const int32 Size = Inventory->GetSizeLinear();
        for (int32 Index = 0; Index < Size; ++Index)
        {
            FInventoryStack Stack;
            if (!Inventory->GetStackFromIndex(Index, Stack)) continue;
            if (!Stack.HasItems() || !Stack.Item.GetItemClass()) continue;
            Out.FindOrAdd(Stack.Item.GetItemClass()) += Stack.NumItems;
        }
    }

    FDTSItem DTSMostHeld(const TMap<FDTSItem, int64>& Held)
    {
        FDTSItem Best = nullptr;
        int64 BestNum = 0;
        for (const TPair<FDTSItem, int64>& Pair : Held)
        {
            if (Pair.Value > BestNum) { BestNum = Pair.Value; Best = Pair.Key; }
        }
        return Best;
    }

    /**
     * What this dock handles, according to itself. The station's inventory is the witness that
     * cannot be argued with: it holds what trucks bring or what the factory sent, and it is the
     * only thing that tells two ingredients of one recipe apart. A fluid dock has two more
     * witnesses of the same rank: the fluid it currently holds, and the fluid its pipe network
     * carries - a network keeps its fluid type even when every tank on it is empty.
     */
    FDTSItem DTSDockCargoItem(AFGBuildableDockingStation* Station, const TArray<UFGConnectionComponent*>& Ports)
    {
        TMap<FDTSItem, int64> Held;
        DTSTallyInventory(Station->GetInventory(), Held);
        if (const FDTSItem Best = DTSMostHeld(Held)) return Best;

        if (const FDTSItem Fluid = Station->GetCurrentFluidDescriptor()) return Fluid;
        for (UFGConnectionComponent* Port : Ports)
        {
            const UFGPipeConnectionComponent* Pipe = Cast<UFGPipeConnectionComponent>(Port);
            if (!Pipe) continue;
            if (const FDTSItem Fluid = Pipe->GetFluidDescriptor()) return Fluid;
        }
        return nullptr;
    }

    /** What the stores behind a warehouse dock hold, by item, counted in inventory units. */
    void DTSTallyStores(const TSet<AActor*>& Stores, TMap<FDTSItem, int64>& Out)
    {
        for (AActor* Store : Stores)
        {
            if (AFGBuildableStorage* Storage = Cast<AFGBuildableStorage>(Store)) DTSTallyInventory(Storage->GetStorageInventory(), Out);
            else if (AFGBuildableTrainPlatformCargo* Platform = Cast<AFGBuildableTrainPlatformCargo>(Store)) DTSTallyInventory(Platform->GetInventory(), Out);
            else if (AFGBuildablePipeReservoir* Reservoir = Cast<AFGBuildablePipeReservoir>(Store))
            {
                if (const FDTSItem Fluid = Reservoir->GetFluidDescriptor())
                {
                    // Litres, so that a tank compares with the solid stacks next to it.
                    Out.FindOrAdd(Fluid) += FMath::Max<int64>(1, int64(Reservoir->GetFluidContent() * 1000.0f));
                }
            }
        }
    }

    void DTSPickDominant(const FDTSRates& Rates, FDTSItem& OutItem, float& OutRate)
    {
        OutItem = nullptr;
        OutRate = 0.0f;
        for (const TPair<FDTSItem, float>& Pair : Rates)
        {
            if (Pair.Value > OutRate) { OutItem = Pair.Key; OutRate = Pair.Value; }
        }
    }

    /** The entries a dock of this form can carry at all: solids over belts, fluids through pipes. */
    FDTSRates DTSOfForm(const FDTSRates& Rates, bool bFluidDock)
    {
        FDTSRates Out;
        for (const TPair<FDTSItem, float>& Pair : Rates)
        {
            if (DTSIsFluidItem(Pair.Key) == bFluidDock) Out.Add(Pair.Key, Pair.Value);
        }
        return Out;
    }
}

bool FDTTAutoNameScanner::Scan(AFGBuildableDockingStation* Station, const FDTTAutoNameLimits& Limits,
    TSubclassOf<UFGItemDescriptor> Preferred, FDTTAutoNameResult& Out)
{
    Out = FDTTAutoNameResult();
    if (!IsValid(Station)) { Out.SkipReason = TEXT("no-station"); return false; }

    const bool bFluidDock = DTSIsFluidForm(Station->GetDockingStationResourceForm());
    Out.bFluidDock = bFluidDock;

    // A vanilla truck station carries several connectors of each kind and players do use more than
    // one, so every connected belt or pipe is walked, not the first one found. The fuel inputs are
    // not among them: a belt of coal into the fuel slot says nothing about the cargo.
    TArray<UFGConnectionComponent*> DockIns, DockOuts;
    FMyDTTHooks::GetDockPorts(Station, DockIns, DockOuts);

    // Both sides wired says nothing about which way the goods travel, and the switch is the
    // player's in that case.
    if (DockOuts.Num() > 0 && DockIns.Num() > 0) { Out.SkipReason = TEXT("both-sides"); return false; }
    if (DockOuts.Num() == 0 && DockIns.Num() == 0) { Out.SkipReason = bFluidDock ? TEXT("no-pipe") : TEXT("no-belt"); return false; }

    const bool bRequester = DockOuts.Num() > 0;
    const TArray<UFGConnectionComponent*>& DockPorts = bRequester ? DockOuts : DockIns;
    FDTSItem Cargo = DTSDockCargoItem(Station, DockPorts);
    if (Cargo && DTSIsFluidItem(Cargo) != bFluidDock) Cargo = nullptr;
    if (Preferred && DTSIsFluidItem(Preferred) != bFluidDock) Preferred = nullptr;

    FDTSWalk Walk;
    Walk.NodeBudget = FMath::Max(16, Limits.MaxNodes);

    TSet<AActor*> Machines, Stores;
    TSet<FDTSItem> PortVotes;
    FDTSWalkSpec Spec;
    Spec.Forward = bRequester ? EDTSPort::Producer : EDTSPort::Consumer;
    Spec.MachinePort = bRequester ? EDTSPort::Consumer : EDTSPort::Producer;
    // The first pass runs unfiltered, because a smart splitter's filter can only be asked about a
    // known item; once the item is decided the walk is taken again with the filters on.
    DTSWalk(DTSSeeds(DockPorts), Spec, Walk, [&](AActor* Machine, UFGConnectionComponent* Port)
    {
        Machines.Add(Machine);
        if (bRequester)
        {
            if (const FDTSItem Voted = DTSPortIngredient(Cast<AFGBuildableManufacturer>(Machine), Cast<UFGFactoryConnectionComponent>(Port)))
                PortVotes.Add(Voted);
        }
    }, &Stores);
    if (Walk.bOverflow) { Out.SkipReason = TEXT("chain-too-large"); return false; }

    if (Machines.Num() == 0)
    {
        if (Stores.Num() == 0) { Out.SkipReason = TEXT("no-machines"); return false; }

        // A warehouse: the belt or pipe ends in storage, so nothing consumes or produces and there
        // is no rate to show. What it carries is what the dock or the stores hold, in that order,
        // with the player's tag between them - and the tag is kept even over an empty store.
        TMap<FDTSItem, int64> Held;
        DTSTallyStores(Stores, Held);
        for (auto It = Held.CreateIterator(); It; ++It)
        {
            if (DTSIsFluidItem(It.Key()) != bFluidDock) It.RemoveCurrent();
        }

        FDTSItem Item = nullptr;
        FString Source;
        if (Cargo) { Item = Cargo; Source = TEXT("cargo"); }
        else if (Preferred) { Item = Preferred; Source = TEXT("tag"); }
        else if (const FDTSItem Most = DTSMostHeld(Held)) { Item = Most; Source = TEXT("held"); Out.bMixed = Held.Num() > 1; }
        if (!Item) { Out.SkipReason = TEXT("warehouse-empty"); return false; }

        Out.bValid = true;
        Out.bWarehouse = true;
        Out.bRequester = bRequester;
        Out.ItemClass = Item;
        Out.RatePerMinute = 0.0f;
        Out.DecidedBy = Source;
        Out.StoreCount = Stores.Num();
        Out.HeldKinds = Held.Num();
        for (AActor* Store : Stores) Out.Machines.Add(Store);
        return true;
    }

    FDTSRates Rates;
    for (AActor* Machine : Machines)
    {
        if (bRequester) DTSAddDemand(Machine, Rates);
        else DTSAddSupply(Machine, Rates);
    }

    // A solid dock cannot carry the water a refinery also wants, and a fluid dock cannot carry its
    // bauxite: only the entries of the dock's own form are candidates for its name.
    const FDTSRates Wanted = DTSOfForm(Rates, bFluidDock);
    if (Wanted.Num() == 0) { Out.SkipReason = TEXT("form-mismatch"); return false; }

    FDTSItem Item = nullptr;
    float Rate = 0.0f;
    DTSPickDominant(Wanted, Item, Rate);
    bool bMixed = Wanted.Num() > 1;
    FString Source = bMixed ? TEXT("dominant") : TEXT("only-one");

    // The cargo wins over the arithmetic whenever the chain has a use for it: a station feeding an
    // assembler with two ingredients carries exactly one of them, and only the station knows which.
    bool bDecided = false;
    if (Cargo && Wanted.Contains(Cargo))
    {
        Item = Cargo;
        Rate = Wanted[Cargo];
        bMixed = false;
        Source = TEXT("cargo");
        bDecided = true;
    }
    else if (Preferred && Wanted.Contains(Preferred))
    {
        // An empty dock says nothing, but its tag does. Keeping the player's answer beats picking
        // the biggest number on a chain that feeds several things.
        Item = Preferred;
        Rate = Wanted[Preferred];
        bMixed = false;
        Source = TEXT("tag");
        bDecided = true;
    }

    // Ask the factory itself: whatever this chain needs and no belt or pipe already brings is what
    // the trucks are for. A foundry fed scrap by its own refineries and silica by nobody is a
    // silica station, and that is a fact about the world rather than a guess about wiring. Two
    // docks feeding one assembler leave two ingredients unsupplied and stay ambiguous, which is
    // honest.
    //
    // The walk backwards is not cheap, so it runs when the answer is still open - or when someone
    // asked to be told everything, which is what /dtt autoname and its preview do.
    if (bRequester && (!bDecided || Limits.bExplainAll))
    {
        FDTSWalk Back;
        Back.NodeBudget = FMath::Max(16, Limits.MaxNodes);
        FDTSRates Supplied;
        DTSCollectSupply(Machines, Back, Supplied);

        // Covered means covered by the numbers, not merely present: an aluminium plant makes some
        // of its own silica as a by-product and still lives on trucks for the rest, and an
        // ingredient that arrives half-fed is exactly the one a station is there to carry.
        for (const TPair<FDTSItem, float>& Pair : Wanted)
        {
            const float* Have = Supplied.Find(Pair.Key);
            if (Have && *Have >= Pair.Value * 0.99f) continue;
            Out.UnsuppliedItem = Pair.Key;
            ++Out.UnsuppliedCount;
        }

        if (!bDecided && Out.UnsuppliedCount == 1)
        {
            Item = Out.UnsuppliedItem;
            Rate = Wanted[Out.UnsuppliedItem];
            bMixed = false;
            Source = TEXT("unsupplied");
            bDecided = true;
        }
    }

    if (!bDecided && PortVotes.Num() == 1)
    {
        // Nothing left but the wiring: the port a belt lands on carries the slot, and so the
        // ingredient, it reaches. Taken, and marked as the guess it is.
        const FDTSItem Voted = *PortVotes.CreateConstIterator();
        if (const float* Asked = Wanted.Find(Voted))
        {
            Item = Voted;
            Rate = *Asked;
            Out.bFromPort = true;
            Source = TEXT("port");
        }
    }

    // What the other signals would have said, kept for the log: a dock whose cargo leaves no doubt
    // is the only place where a guess can be checked against the truth.
    if (PortVotes.Num() == 1) Out.PortItem = *PortVotes.CreateConstIterator();
    Out.PortVoteCount = PortVotes.Num();
    if (!Item) { Out.SkipReason = TEXT("no-machines"); return false; }

    if (bRequester)
    {
        // Now that the item is known the filters can speak, so the reachable set is taken again.
        Machines.Reset();
        Walk.NodeBudget = FMath::Max(16, Limits.MaxNodes);
        Spec.Filter = Item;
        DTSWalk(DTSSeeds(DockPorts), Spec, Walk, [&Machines](AActor* Machine, UFGConnectionComponent*) { Machines.Add(Machine); }, nullptr);
        if (Walk.bOverflow) { Out.SkipReason = TEXT("chain-too-large"); return false; }

        // Only the machines that actually want this item belong to this station's chain.
        TSet<AActor*> Consumers;
        Rate = 0.0f;
        for (AActor* Machine : Machines)
        {
            FDTSRates One;
            DTSAddDemand(Machine, One);
            if (const float* Asked = One.Find(Item))
            {
                Rate += *Asked;
                Consumers.Add(Machine);
            }
        }
        if (Consumers.Num() == 0 || Rate <= 0.0f) { Out.SkipReason = TEXT("no-machines"); return false; }

        // Every machine walks its own way out, so a manifold shared by ten refineries is crossed
        // ten times over; the products get a budget of their own, wide enough for that, and running
        // out of it shortens the list instead of failing the station.
        FDTSRates Products;
        TSet<AActor*> VisitedMachines;
        FDTSWalk ProductWalk;
        ProductWalk.NodeBudget = FMath::Max(16, Limits.MaxNodes) * 16;
        for (AActor* Machine : Consumers)
        {
            if (VisitedMachines.Num() >= Limits.MaxMachines || ProductWalk.bOverflow) break;
            DTSCollectFinalProducts(Machine, ProductWalk, Products, Out.PowerMW, VisitedMachines, 0);
        }
        if (ProductWalk.bOverflow)
        {
            Out.bMoreProducts = true;
            UE_LOG(LogDTTAutoName, Verbose, TEXT("AUTONAME_PRODUCTS_CUT Station=%s Machines=%d"), *Station->GetName(), VisitedMachines.Num());
        }

        Products.ValueSort([](float A, float B) { return A > B; });
        for (const TPair<FDTSItem, float>& Pair : Products)
        {
            if (Out.ProductClasses.Num() >= FMath::Max(1, Limits.MaxProducts)) { Out.bMoreProducts = true; break; }
            Out.ProductClasses.Add(Pair.Key);
        }

        for (AActor* Machine : Consumers) Out.Machines.Add(Machine);
    }
    else
    {
        for (AActor* Machine : Machines)
        {
            FDTSRates One;
            DTSAddSupply(Machine, One);
            if (One.Contains(Item)) Out.Machines.Add(Machine);
        }

        // The number on a collecting station is what comes down the belt to it, not what the line
        // makes: the ten refineries behind one dock also fed four constructors and a sink through
        // five splitters, and the player who had written 100 on that dock was right where the
        // walk said 200. Pipes have no direction to discount along, so a fluid dock keeps the
        // network's whole output.
        Out.LineRatePerMinute = Rate;
        if (!bFluidDock)
        {
            FDTSWalk Flow;
            Flow.NodeBudget = FMath::Max(16, Limits.MaxNodes) * 16;
            TSet<AActor*> Visited;
            Visited.Add(Station);
            float Reaching = 0.0f;
            for (UFGConnectionComponent* Seed : DTSSeeds(DockPorts))
            {
                Reaching += DTSFlow(Seed, Item, EDTSFlow::Supply, Flow, Visited, 0);
            }
            if (Flow.bOverflow)
            {
                UE_LOG(LogDTTAutoName, Verbose, TEXT("AUTONAME_SHARE_CUT Station=%s Line=%.1f"), *Station->GetName(), Rate);
            }
            else
            {
                Rate = FMath::Min(Rate, Reaching);
            }
        }
    }

    Out.bRequester = bRequester;
    Out.ItemClass = Item;
    Out.RatePerMinute = Rate;
    // Without cargo to go by, several candidate items on one chain means the biggest one was taken,
    // and a port's word is a guess as well: the game does not make the wiring mean anything.
    Out.bMixed = bMixed || Out.bFromPort;
    Out.DecidedBy = Source;

    // How this station was decided, and what the ports would have said either way. A dock whose
    // cargo leaves no doubt is the only place where the port's word can be checked against the
    // truth, so it reports both. Measured over one built world on 16.09.2026: 19 agreed, 18
    // disagreed - a coin flip, and the failures came in clean swapped pairs, two docks feeding one
    // assembler through each other's port. That is why the port is the last word and not the first,
    // and why it still prints '?'.
    UE_LOG(LogDTTAutoName, Verbose, TEXT("AUTONAME_PORTCHECK Item=%s Source=%s Port=%s Votes=%d Unsupplied=%s Candidates=%d"),
        *UFGItemDescriptor::GetItemName(Item).ToString(), *Source,
        Out.PortItem ? *UFGItemDescriptor::GetItemName(Out.PortItem).ToString() : TEXT("-"),
        Out.PortVoteCount,
        Out.UnsuppliedItem ? *UFGItemDescriptor::GetItemName(Out.UnsuppliedItem).ToString() : TEXT("-"),
        Out.UnsuppliedCount);

    Out.bValid = true;
    return true;
}
