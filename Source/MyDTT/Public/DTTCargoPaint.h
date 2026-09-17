#pragma once

#include "CoreMinimal.h"
#include "Templates/SubclassOf.h"
#include "FGFactoryColoringTypes.h"

class UFGItemDescriptor;

/** The two colours a truck wears for one cargo: the item's own two, as its icon has them. */
struct FDTTCargoColors
{
    FLinearColor Primary = FLinearColor::White;
    FLinearColor Secondary = FLinearColor::White;

    bool Equals(const FDTTCargoColors& Other, float Tolerance = 0.01f) const
    {
        return Primary.Equals(Other.Primary, Tolerance) && Secondary.Equals(Other.Secondary, Tolerance);
    }
};

/**
 * The colours a truck wears for what it carries.
 *
 * A vehicle's paint is AFGVehicle::mCustomizationData, replicated and saved by the game, so the
 * server writes it and every client - with or without this mod - sees it. The paint itself is the
 * game's own "custom" swatch with an override colour pair, which is what the paint gun writes when
 * a player picks colours outside the slots. A truck therefore wears two colours, like any painted
 * vehicle: for an item whose icon is two-coloured those are its two colours, and for a plain one
 * the trim is the main colour darkened.
 *
 * Where the colours come from, in order: a fluid or gas has one on its descriptor; a vanilla solid
 * item has a pair baked into DTTCargoPaintTable.inc, taken from its icon; anything else gets a hue
 * from the hash of its class name. The table is baked rather than read at runtime because a
 * dedicated server's cook keeps no texture pixels at all - see DumpItemColors.
 */
class MYDTT_API FDTTCargoPaint
{
public:
    /** The colours for this cargo. Never fails: an unknown item still gets a stable hue. */
    static FDTTCargoColors ColorsFor(TSubclassOf<UFGItemDescriptor> Item);

    /** Base with the custom swatch and the cargo's colour pair written over it. */
    static FFactoryCustomizationData MakePaint(const FFactoryCustomizationData& Base, const FDTTCargoColors& Colors);

    /** True when Data already is the paint MakePaint would produce for Colors. */
    static bool IsPainted(const FFactoryCustomizationData& Data, const FDTTCargoColors& Colors);

    /** False when the game's custom swatch could not be loaded; painting is then impossible. */
    static bool IsAvailable();

    /** "Desc_OreIron_C" -> its colour pair; exposed for the dump command and tests. */
    static const TMap<FName, FDTTCargoColors>& BakedTable();
};
