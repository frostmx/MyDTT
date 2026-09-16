#pragma once

#include "CoreMinimal.h"
#include "Templates/SubclassOf.h"

class UFGItemDescriptor;

/** One entry of the language list: the game's culture code and what to print for it. */
struct FDTTLanguageOption
{
    FString Code;   // "en", "ru", "zh-Hans" - as the game names its cultures
    FString Label;  // "Deutsch (German)"
};

/**
 * Item names in a language of the player's choosing, taken from the game's own translations.
 *
 * A dedicated server has no language of its own and a player may write tags in one language while
 * playing the UI in another, so the auto-namer cannot simply ask FText for the current culture.
 * The game keeps its item names in string tables (Items_Data, key "Materials/Bauxite") and ships
 * the translations as one .locres per culture under Content/Localization/AllStringTables - 54 of
 * them, on the client and on the dedicated server alike. This reads those files directly, without
 * switching the process culture, and remembers what it found.
 *
 * Game thread only. Owned by the subsystem, one per world.
 */
class MYDTT_API FDTTItemNames
{
public:
    static const TCHAR* DefaultLanguage() { return TEXT("en"); }

    /** "en" first, then every language the options menu offers and a .locres exists for. */
    static TArray<FDTTLanguageOption> ListLanguages();
    /** "en" always; any other code needs a compiled .locres in one of the game's localization paths. */
    static bool HasLanguage(const FString& Code);
    /** Trimmed, and "en" for anything the game cannot name items in. */
    static FString NormalizeLanguage(const FString& Code);

    /** The item's name in Code; the English source string when the language has no entry for it. */
    FString NameOf(TSubclassOf<UFGItemDescriptor> Item, const FString& Code, UObject* WorldContext);

    /**
     * The item whose name in some language equals Name, case-insensitively. Looks in English, the
     * preferred language and the process culture first; on a miss every other language is loaded
     * once, so a "(Bauxit)" written on a German client resolves on an English server. Null means
     * the word is the player's own and not a game name at all.
     */
    TSubclassOf<UFGItemDescriptor> Resolve(const FString& Name, const FString& PreferredCode, UObject* WorldContext);

private:
    struct FTable
    {
        bool bBuilt = false;
        TMap<TSubclassOf<UFGItemDescriptor>, FString> Names;
    };

    const TArray<TSubclassOf<UFGItemDescriptor>>& AllItems(UObject* WorldContext);
    const FTable& TableFor(const FString& Code, UObject* WorldContext);
    void MergeIntoResolver(const FString& Code, UObject* WorldContext);

    TArray<TSubclassOf<UFGItemDescriptor>> Items;
    bool bItemsCollected = false;
    TMap<FString, FTable> Tables;
    /** Lower-cased name in any language merged so far -> item. First writer wins. */
    TMap<FString, TSubclassOf<UFGItemDescriptor>> ClassByName;
    TSet<FString> MergedCodes;
    bool bAllLanguagesMerged = false;
};
