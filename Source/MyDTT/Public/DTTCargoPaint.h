#pragma once

#include "CoreMinimal.h"
#include "Templates/SubclassOf.h"
#include "FGFactoryColoringTypes.h"

class UFGItemDescriptor;

/**
 * The colour a truck wears for what it carries.
 *
 * A vehicle's paint is AFGVehicle::mCustomizationData, replicated and saved by the game, so the
 * server writes it and every client - with or without this mod - sees it. The colour itself is the
 * game's own "custom" swatch with an override colour, which is what the paint gun writes when a
 * player picks a colour outside the slots.
 *
 * Where the colour comes from, in order: a fluid or gas has one on its descriptor; a vanilla solid
 * item has one baked into DTTCargoPaintTable.inc, the average of its icon; anything else gets a
 * hue from the hash of its class name. The table is baked rather than read at runtime because a
 * dedicated server's cook keeps no texture pixels at all - see DumpItemColors.
 */
class MYDTT_API FDTTCargoPaint
{
public:
    /** The colour for this cargo. Never fails: an unknown item still gets a stable hue. */
    static FLinearColor ColorFor(TSubclassOf<UFGItemDescriptor> Item);

    /** Base with the custom swatch and Primary (plus a darker secondary) written over it. */
    static FFactoryCustomizationData MakePaint(const FFactoryCustomizationData& Base, const FLinearColor& Primary);

    /** True when Data already is the paint MakePaint would produce for Primary. */
    static bool IsPainted(const FFactoryCustomizationData& Data, const FLinearColor& Primary);

    /** False when the game's custom swatch could not be loaded; painting is then impossible. */
    static bool IsAvailable();

    /** "Desc_OreIron_C" -> colour for the baked table; exposed for the dump command and tests. */
    static const TMap<FName, FLinearColor>& BakedTable();
};
