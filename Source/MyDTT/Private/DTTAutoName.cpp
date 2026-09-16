#include "DTTAutoName.h"

#include "FGFactoryConnectionComponent.h"
#include "FGInventoryComponent.h"
#include "FGRecipe.h"
#include "Buildables/FGBuildableConveyorAttachment.h"
#include "Buildables/FGBuildableConveyorBase.h"
#include "Buildables/FGBuildableConveyorMonitor.h"
#include "Buildables/FGBuildableDockingStation.h"
#include "Buildables/FGBuildableManufacturer.h"
#include "Buildables/FGBuildableResourceExtractor.h"
#include "Buildables/FGBuildableSplitterSmart.h"
#include "Buildables/FGBuildableStorage.h"
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

    bool DTSIsFluidItem(FDTSItem Item)
    {
        if (!Item) return false;
        const EResourceForm Form = UFGItemDescriptor::GetForm(Item);
        return Form == EResourceForm::RF_LIQUID || Form == EResourceForm::RF_GAS;
    }

    /**
     * Only these carry items through without consuming them. Everything else - another station, a
     * train platform, the sink, anything unknown - ends the branch. A container looks exactly like
     * a splitter from the outside, so this has to be a whitelist, not "has two connections".
     */
    bool DTSIsPassThrough(const AActor* Actor)
    {
        return Actor->IsA<AFGBuildableConveyorBase>() ||
               Actor->IsA<AFGBuildableConveyorAttachment>() ||
               Actor->IsA<AFGBuildableConveyorMonitor>();
    }

    /**
     * A container between a station and its factory is a buffer, not a consumer: measured in the
     * live world, most docks feed or are fed through one, and stopping there answered "no machines"
     * for almost every station. It stays a terminator for the final-product hunt, where passing
     * through it would wander into the next factory over.
     */
    bool DTSIsBuffer(const AActor* Actor)
    {
        return Actor->IsA<AFGBuildableStorage>();
    }

    void DTSGetFactoryConnections(AActor* Actor, TArray<UFGFactoryConnectionComponent*>& Out)
    {
        Out.Reset();
        if (IsValid(Actor)) Actor->GetComponents<UFGFactoryConnectionComponent>(Out);
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
    bool DTSSplitterAllows(AActor* Attachment, const UFGFactoryConnectionComponent* Out, FDTSItem Item)
    {
        const AFGBuildableSplitterSmart* Smart = Cast<AFGBuildableSplitterSmart>(Attachment);
        if (!Smart || !Item) return true;

        const TArray<FSplitterSortRule> Rules = Smart->GetSortRules();
        if (Rules.Num() == 0) return true;

        const int32 Index = DTSOutputIndexFromName(Out);
        if (Index == INDEX_NONE) return true;

        bool bHasOwnRule = false, bWild = false, bAnyUndefined = false, bNone = false;
        bool bExactHere = false, bNamedElsewhere = false;
        for (const FSplitterSortRule& Rule : Rules)
        {
            UClass* RuleClass = Rule.ItemClass.Get();
            if (!RuleClass) continue;
            const bool bMine = Rule.OutputIndex == Index;
            const bool bWildClass = RuleClass->IsChildOf(UFGWildCardDescriptor::StaticClass()) ||
                                    RuleClass->IsChildOf(UFGOverflowDescriptor::StaticClass());
            const bool bUndefClass = RuleClass->IsChildOf(UFGAnyUndefinedDescriptor::StaticClass());
            const bool bNoneClass = RuleClass->IsChildOf(UFGNoneDescriptor::StaticClass());

            if (!bWildClass && !bUndefClass && !bNoneClass && RuleClass == Item.Get())
            {
                if (bMine) bExactHere = true; else bNamedElsewhere = true;
            }
            if (bMine)
            {
                bHasOwnRule = true;
                bWild |= bWildClass;
                bAnyUndefined |= bUndefClass;
                bNone |= bNoneClass;
            }
        }

        if (bExactHere) return true;
        if (bWild) return true;                       // Any, and Overflow counted as passable
        if (bAnyUndefined) return !bNamedElsewhere;
        if (bNone) return false;
        return !bHasOwnRule;                          // an output nobody filtered carries everything
    }

    /**
     * Everything this manufacturer eats, per minute, by item.
     *
     * Deliberately not asked per port. The plan assumed an input connection's
     * GetInventoryAccessIndex() names the ingredient that arrives on that belt; measured in the live
     * world on 16.09.2026 it does not. A circuit-board assembler had the copper-sheet belt on the
     * connector bound to the plastic slot and ran perfectly - the game sorts an arriving item into
     * whichever slot accepts it, no matter which connector it came in by. So a machine is read as
     * wanting all of its ingredients, and which one a station actually carries is decided by the
     * dock's own cargo.
     */
    void DTSAddDemand(AFGBuildableManufacturer* Manufacturer, FDTSRates& Out)
    {
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

    /** Everything this machine makes, per minute, by item. */
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

    /** The connection on the far end of a belt, whichever end we came in by. */
    UFGFactoryConnectionComponent* DTSBeltFarSide(AFGBuildableConveyorBase* Belt, const UFGFactoryConnectionComponent* Entered)
    {
        UFGFactoryConnectionComponent* Zero = Belt->GetConnection0();
        UFGFactoryConnectionComponent* One = Belt->GetConnection1();
        return Entered == Zero ? One : Zero;
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
     * Downstream from a connection, collecting the machines at the far end.
     * Filter null on the first pass (the item is still unknown), set on the second.
     * OutPortVotes, when given, gathers what the ports say - see DTSPortIngredient.
     */
    void DTSWalkDemand(UFGFactoryConnectionComponent* Start, FDTSItem Filter, FDTSWalk& Walk,
        TSet<AActor*>& OutMachines, bool bThroughBuffers = true, TSet<FDTSItem>* OutPortVotes = nullptr)
    {
        TSet<AActor*> Visited;
        TArray<UFGFactoryConnectionComponent*> Frontier;
        Frontier.Add(Start);

        while (Frontier.Num() > 0)
        {
            UFGFactoryConnectionComponent* In = Frontier.Pop(EAllowShrinking::No);
            if (!IsValid(In)) continue;

            AActor* Node = In->GetOwner();
            if (!IsValid(Node) || Visited.Contains(Node)) continue;
            Visited.Add(Node);

            if (--Walk.NodeBudget <= 0) { Walk.bOverflow = true; return; }

            if (AFGBuildableManufacturer* Manufacturer = Cast<AFGBuildableManufacturer>(Node))
            {
                OutMachines.Add(Manufacturer);
                if (OutPortVotes)
                {
                    if (const FDTSItem Voted = DTSPortIngredient(Manufacturer, In)) OutPortVotes->Add(Voted);
                }
                continue;
            }

            const bool bBuffer = bThroughBuffers && DTSIsBuffer(Node);
            if (!bBuffer && !DTSIsPassThrough(Node)) continue;

            if (AFGBuildableConveyorBase* Belt = Cast<AFGBuildableConveyorBase>(Node))
            {
                UFGFactoryConnectionComponent* Far = DTSBeltFarSide(Belt, In);
                if (IsValid(Far) && Far->IsConnected()) Frontier.Add(Far->GetConnection());
                continue;
            }

            TArray<UFGFactoryConnectionComponent*> Conns;
            DTSGetFactoryConnections(Node, Conns);
            for (UFGFactoryConnectionComponent* Conn : Conns)
            {
                if (!IsValid(Conn) || !Conn->IsConnected()) continue;
                if (Conn->GetDirection() != EFactoryConnectionDirection::FCD_OUTPUT) continue;
                if (!bBuffer && Filter && !DTSSplitterAllows(Node, Conn, Filter)) continue;
                Frontier.Add(Conn->GetConnection());
            }
        }
    }

    /** Upstream from a connection, collecting the machines behind it. */
    void DTSWalkSupply(UFGFactoryConnectionComponent* Start, FDTSWalk& Walk, TSet<AActor*>& OutMachines)
    {
        TSet<AActor*> Visited;
        TArray<UFGFactoryConnectionComponent*> Frontier;
        Frontier.Add(Start);

        while (Frontier.Num() > 0)
        {
            UFGFactoryConnectionComponent* OutConn = Frontier.Pop(EAllowShrinking::No);
            if (!IsValid(OutConn)) continue;

            AActor* Node = OutConn->GetOwner();
            if (!IsValid(Node) || Visited.Contains(Node)) continue;
            Visited.Add(Node);

            if (--Walk.NodeBudget <= 0) { Walk.bOverflow = true; return; }

            if (Node->IsA<AFGBuildableManufacturer>() || Node->IsA<AFGBuildableResourceExtractor>())
            {
                OutMachines.Add(Node);
                continue;
            }

            if (!DTSIsBuffer(Node) && !DTSIsPassThrough(Node)) continue;

            if (AFGBuildableConveyorBase* Belt = Cast<AFGBuildableConveyorBase>(Node))
            {
                UFGFactoryConnectionComponent* Far = DTSBeltFarSide(Belt, OutConn);
                if (IsValid(Far) && Far->IsConnected()) Frontier.Add(Far->GetConnection());
                continue;
            }

            TArray<UFGFactoryConnectionComponent*> Conns;
            DTSGetFactoryConnections(Node, Conns);
            for (UFGFactoryConnectionComponent* Conn : Conns)
            {
                if (!IsValid(Conn) || !Conn->IsConnected()) continue;
                if (Conn->GetDirection() != EFactoryConnectionDirection::FCD_INPUT) continue;
                Frontier.Add(Conn->GetConnection());
            }
        }
    }

    /**
     * What already arrives at these machines on a belt from something that makes it.
     *
     * The strongest thing that can be said about an empty dock: whatever the chain needs and
     * nobody else delivers is what this dock is for. A truck station upstream ends the walk
     * silently and supplies nothing, so two docks feeding one assembler stay ambiguous - which is
     * honest, that case really is a coin flip.
     */
    void DTSCollectBeltSupply(const TSet<AActor*>& Machines, FDTSWalk& Walk, FDTSRates& Out)
    {
        // One walk for the whole chain, not one per machine: twenty foundries share the belts
        // behind them, and walking each separately spends the node budget on the same conveyors
        // twenty times over - which made the answer "I cannot tell" on exactly the big factories
        // this is for.
        TSet<AActor*> Visited;
        TArray<UFGFactoryConnectionComponent*> Frontier;
        for (AActor* Machine : Machines)
        {
            Visited.Add(Machine);
            TArray<UFGFactoryConnectionComponent*> Conns;
            DTSGetFactoryConnections(Machine, Conns);
            for (UFGFactoryConnectionComponent* Conn : Conns)
            {
                if (!IsValid(Conn) || !Conn->IsConnected()) continue;
                if (Conn->GetDirection() != EFactoryConnectionDirection::FCD_INPUT) continue;
                Frontier.Add(Conn->GetConnection());
            }
        }

        while (Frontier.Num() > 0)
        {
            UFGFactoryConnectionComponent* OutConn = Frontier.Pop(EAllowShrinking::No);
            if (!IsValid(OutConn)) continue;

            AActor* Node = OutConn->GetOwner();
            if (!IsValid(Node) || Visited.Contains(Node)) continue;
            Visited.Add(Node);

            if (--Walk.NodeBudget <= 0) { Walk.bOverflow = true; return; }

            if (Node->IsA<AFGBuildableManufacturer>() || Node->IsA<AFGBuildableResourceExtractor>())
            {
                FDTSRates Made;
                DTSAddSupply(Node, Made);
                for (const TPair<FDTSItem, float>& Pair : Made) Out.FindOrAdd(Pair.Key) += Pair.Value;
                continue;
            }

            if (!DTSIsBuffer(Node) && !DTSIsPassThrough(Node)) continue;

            if (AFGBuildableConveyorBase* Belt = Cast<AFGBuildableConveyorBase>(Node))
            {
                UFGFactoryConnectionComponent* Far = DTSBeltFarSide(Belt, OutConn);
                if (IsValid(Far) && Far->IsConnected()) Frontier.Add(Far->GetConnection());
                continue;
            }

            TArray<UFGFactoryConnectionComponent*> Conns;
            DTSGetFactoryConnections(Node, Conns);
            for (UFGFactoryConnectionComponent* Conn : Conns)
            {
                if (!IsValid(Conn) || !Conn->IsConnected()) continue;
                if (Conn->GetDirection() != EFactoryConnectionDirection::FCD_INPUT) continue;
                Frontier.Add(Conn->GetConnection());
            }
        }
    }

    /**
     * From a machine forward to whatever the chain finally makes. A branch that reaches another
     * manufacturer keeps going; a branch that dies at a container, a station or a bare belt end
     * makes the last manufacturer on it the final one.
     */
    void DTSCollectFinalProducts(AActor* Machine, FDTSWalk& Walk, FDTSRates& Out, TSet<AActor*>& VisitedMachines, int32 Depth)
    {
        if (Depth > 16 || !IsValid(Machine) || VisitedMachines.Contains(Machine)) return;
        VisitedMachines.Add(Machine);

        AFGBuildableManufacturer* Manufacturer = Cast<AFGBuildableManufacturer>(Machine);
        if (!Manufacturer) return;

        FDTSRates OwnProducts;
        DTSAddSupply(Manufacturer, OwnProducts);

        TArray<UFGFactoryConnectionComponent*> Conns;
        DTSGetFactoryConnections(Machine, Conns);

        bool bFedAnother = false;
        for (UFGFactoryConnectionComponent* Conn : Conns)
        {
            if (!IsValid(Conn) || !Conn->IsConnected()) continue;
            if (Conn->GetDirection() != EFactoryConnectionDirection::FCD_OUTPUT) continue;

            TSet<AActor*> Downstream;
            DTSWalkDemand(Conn->GetConnection(), nullptr, Walk, Downstream, false);
            for (AActor* Next : Downstream)
            {
                bFedAnother = true;
                DTSCollectFinalProducts(Next, Walk, Out, VisitedMachines, Depth + 1);
            }
            if (Walk.bOverflow) break;
        }

        if (!bFedAnother)
        {
            for (const TPair<FDTSItem, float>& Pair : OwnProducts) Out.FindOrAdd(Pair.Key) += Pair.Value;
        }
    }

    /**
     * What this dock handles, according to itself. The station's inventory is the witness that
     * cannot be argued with: it holds what trucks bring or what the factory sent, and it is the
     * only thing that tells two ingredients of one recipe apart.
     */
    FDTSItem DTSDockCargoItem(AFGBuildableDockingStation* Station)
    {
        UFGInventoryComponent* Inventory = Station->GetInventory();
        if (!IsValid(Inventory)) return nullptr;

        FDTSItem Best = nullptr;
        int32 BestNum = 0;
        const int32 Size = Inventory->GetSizeLinear();
        for (int32 Index = 0; Index < Size; ++Index)
        {
            FInventoryStack Stack;
            if (!Inventory->GetStackFromIndex(Index, Stack)) continue;
            if (!Stack.HasItems() || !Stack.Item.GetItemClass()) continue;
            if (Stack.NumItems > BestNum)
            {
                BestNum = Stack.NumItems;
                Best = Stack.Item.GetItemClass();
            }
        }
        return Best;
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
}

bool FDTTAutoNameScanner::Scan(AFGBuildableDockingStation* Station, const FDTTAutoNameLimits& Limits,
    TSubclassOf<UFGItemDescriptor> Preferred, FDTTAutoNameResult& Out)
{
    Out = FDTTAutoNameResult();
    if (!IsValid(Station)) { Out.SkipReason = TEXT("no-station"); return false; }

    TArray<UFGFactoryConnectionComponent*> Conns;
    DTSGetFactoryConnections(Station, Conns);

    // A vanilla truck station carries several connectors of each kind and players do use more than
    // one, so every connected belt is walked, not the first one found.
    TArray<UFGFactoryConnectionComponent*> DockOuts, DockIns;
    for (UFGFactoryConnectionComponent* Conn : Conns)
    {
        if (!IsValid(Conn) || !Conn->IsConnected()) continue;
        if (Conn->GetDirection() == EFactoryConnectionDirection::FCD_OUTPUT) DockOuts.Add(Conn);
        else if (Conn->GetDirection() == EFactoryConnectionDirection::FCD_INPUT) DockIns.Add(Conn);
    }

    // Both sides wired says nothing about which way the goods travel, and the switch is the
    // player's in that case.
    if (DockOuts.Num() > 0 && DockIns.Num() > 0) { Out.SkipReason = TEXT("belts-on-both-sides"); return false; }
    if (DockOuts.Num() == 0 && DockIns.Num() == 0) { Out.SkipReason = TEXT("no-belt"); return false; }

    const bool bRequester = DockOuts.Num() > 0;
    const FDTSItem Cargo = DTSDockCargoItem(Station);

    FDTSWalk Walk;
    Walk.NodeBudget = FMath::Max(16, Limits.MaxNodes);

    TSet<AActor*> Machines;
    TSet<FDTSItem> PortVotes;
    if (bRequester)
    {
        // The first pass runs unfiltered, because a smart splitter's filter can only be asked about
        // a known item; once the item is decided the walk is taken again with the filters on.
        for (UFGFactoryConnectionComponent* DockOut : DockOuts)
            DTSWalkDemand(DockOut->GetConnection(), nullptr, Walk, Machines, true, &PortVotes);
    }
    else
    {
        for (UFGFactoryConnectionComponent* DockIn : DockIns) DTSWalkSupply(DockIn->GetConnection(), Walk, Machines);
    }
    if (Walk.bOverflow) { Out.SkipReason = TEXT("chain-too-large"); return false; }
    if (Machines.Num() == 0) { Out.SkipReason = TEXT("no-machines"); return false; }

    FDTSRates Rates;
    for (AActor* Machine : Machines)
    {
        if (bRequester) DTSAddDemand(Cast<AFGBuildableManufacturer>(Machine), Rates);
        else DTSAddSupply(Machine, Rates);
    }

    FDTSItem Item = nullptr;
    float Rate = 0.0f;
    DTSPickDominant(Rates, Item, Rate);
    bool bMixed = Rates.Num() > 1;
    FString Source = bMixed ? TEXT("dominant") : TEXT("only-one");

    // The cargo wins over the arithmetic whenever the chain has a use for it: a station feeding an
    // assembler with two ingredients carries exactly one of them, and only the station knows which.
    bool bDecided = false;
    if (Cargo && Rates.Contains(Cargo))
    {
        Item = Cargo;
        Rate = Rates[Cargo];
        bMixed = false;
        Source = TEXT("cargo");
        bDecided = true;
    }
    else if (Preferred && Rates.Contains(Preferred))
    {
        // An empty dock says nothing, but its tag does. Keeping the player's answer beats picking
        // the biggest number on a chain that feeds several things.
        Item = Preferred;
        Rate = Rates[Preferred];
        bMixed = false;
        Source = TEXT("tag");
        bDecided = true;
    }

    // Ask the factory itself: whatever this chain needs and no belt already brings is what the
    // trucks are for. A foundry fed scrap by its own refineries and silica by nobody is a silica
    // station, and that is a fact about the world rather than a guess about wiring. Two docks
    // feeding one assembler leave two ingredients unsupplied and stay ambiguous, which is honest.
    //
    // The walk backwards is not cheap, so it runs when the answer is still open - or when someone
    // asked to be told everything, which is what /dtt autoname and its preview do.
    if (bRequester && (!bDecided || Limits.bExplainAll))
    {
        FDTSWalk Back;
        Back.NodeBudget = FMath::Max(16, Limits.MaxNodes);
        FDTSRates ByBelt;
        DTSCollectBeltSupply(Machines, Back, ByBelt);

        // Covered means covered by the numbers, not merely present: an aluminium plant makes some
        // of its own silica as a by-product and still lives on trucks for the rest, and an
        // ingredient that arrives half-fed is exactly the one a station is there to carry.
        for (const TPair<FDTSItem, float>& Pair : Rates)
        {
            // A fluid arrives by pipe, which this walk cannot see at all, so it would always look
            // unsupplied. Trucks do not carry it either - naming stops at solids - so it is not a
            // candidate and must not stand in the way of the one that is.
            if (DTSIsFluidItem(Pair.Key)) continue;

            const float* Supplied = ByBelt.Find(Pair.Key);
            if (Supplied && *Supplied >= Pair.Value * 0.99f) continue;
            Out.UnsuppliedItem = Pair.Key;
            ++Out.UnsuppliedCount;
        }

        if (!bDecided && Out.UnsuppliedCount == 1)
        {
            Item = Out.UnsuppliedItem;
            Rate = Rates[Out.UnsuppliedItem];
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
        if (const float* Wanted = Rates.Find(Voted))
        {
            Item = Voted;
            Rate = *Wanted;
            Out.bFromPort = true;
            Source = TEXT("port");
        }
    }

    // What the other signals would have said, kept for the log: a dock whose cargo leaves no doubt
    // is the only place where a guess can be checked against the truth.
    if (PortVotes.Num() == 1) Out.PortItem = *PortVotes.CreateConstIterator();
    Out.PortVoteCount = PortVotes.Num();
    if (!Item) { Out.SkipReason = TEXT("no-machines"); return false; }
    if (DTSIsFluidItem(Item)) { Out.SkipReason = TEXT("fluid-not-supported"); return false; }

    if (bRequester)
    {
        // Now that the item is known the filters can speak, so the reachable set is taken again.
        Machines.Reset();
        Walk.NodeBudget = FMath::Max(16, Limits.MaxNodes);
        for (UFGFactoryConnectionComponent* DockOut : DockOuts) DTSWalkDemand(DockOut->GetConnection(), Item, Walk, Machines);
        if (Walk.bOverflow) { Out.SkipReason = TEXT("chain-too-large"); return false; }

        // Only the machines that actually want this item belong to this station's chain.
        TSet<AActor*> Consumers;
        Rate = 0.0f;
        for (AActor* Machine : Machines)
        {
            FDTSRates One;
            DTSAddDemand(Cast<AFGBuildableManufacturer>(Machine), One);
            if (const float* Wanted = One.Find(Item))
            {
                Rate += *Wanted;
                Consumers.Add(Machine);
            }
        }
        if (Consumers.Num() == 0 || Rate <= 0.0f) { Out.SkipReason = TEXT("no-machines"); return false; }

        FDTSRates Products;
        TSet<AActor*> VisitedMachines;
        for (AActor* Machine : Consumers)
        {
            if (VisitedMachines.Num() >= Limits.MaxMachines) break;
            DTSCollectFinalProducts(Machine, Walk, Products, VisitedMachines, 0);
        }

        Products.ValueSort([](float A, float B) { return A > B; });
        for (const TPair<FDTSItem, float>& Pair : Products)
        {
            if (Out.Products.Num() >= FMath::Max(1, Limits.MaxProducts)) { Out.bMoreProducts = true; break; }
            Out.Products.Add(UFGItemDescriptor::GetItemName(Pair.Key).ToString());
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

    Out.ResourceName = UFGItemDescriptor::GetItemName(Item).ToString().TrimStartAndEnd();
    if (Out.ResourceName.IsEmpty()) { Out.SkipReason = TEXT("resource-has-no-name"); return false; }

    Out.bValid = true;
    return true;
}
