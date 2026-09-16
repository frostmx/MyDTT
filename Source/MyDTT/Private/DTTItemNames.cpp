#include "DTTItemNames.h"

#include "FGBlueprintFunctionLibrary.h"
#include "FGLocalizationSettings.h"
#include "Engine/DataTable.h"
#include "HAL/FileManager.h"
#include "Internationalization/Culture.h"
#include "Internationalization/Internationalization.h"
#include "Internationalization/StringTableCore.h"
#include "Internationalization/StringTableRegistry.h"
#include "Internationalization/Text.h"
#include "Internationalization/TextLocalizationManager.h"
#include "Internationalization/TextLocalizationResource.h"
#include "Misc/Paths.h"
#include "Resources/FGItemDescriptor.h"

DEFINE_LOG_CATEGORY_STATIC(LogDTTItemNames, Log, All);

namespace
{
    /** "<path>/<code>/<target>.locres" for every game localization target, existing files only. */
    TArray<FString> DTNLocresFiles(const FString& Code)
    {
        TArray<FString> Files;
        for (const FString& Dir : FPaths::GetGameLocalizationPaths())
        {
            const FString Target = FPaths::GetBaseFilename(Dir);
            const FString File = Dir / Code / (Target + TEXT(".locres"));
            if (IFileManager::Get().FileExists(*File)) Files.Add(File);
        }
        return Files;
    }

    /**
     * The code as the game's localization data spells it. The options table names its rows "HR"
     * and "No" where the folders are "hr" and "no"; a pak lookup forgives that, a setting that
     * prints "NamingLanguage=HR" next to "hr" in the log does not.
     */
    FString DTNCanonicalCode(const FString& Code)
    {
        static const TArray<FString> Known = FTextLocalizationManager::Get().GetLocalizedCultureNames(ELocalizationLoadFlags::Game);
        for (const FString& Culture : Known)
        {
            if (Culture.Equals(Code, ESearchCase::IgnoreCase)) return Culture;
        }
        return Code;
    }

    /** The text's localization identity: string-table texts carry their table's namespace. */
    bool DTNTextId(const FText& Text, FTextId& OutId)
    {
        FName TableId;
        FTextKey Key;
        if (FTextInspector::GetTableIdAndKey(Text, TableId, Key))
        {
            const FStringTableConstPtr Table = FStringTableRegistry::Get().FindStringTable(TableId);
            if (!Table.IsValid()) return false;
            OutId = FTextId(FTextKey(Table->GetNamespace()), Key);
            return true;
        }
        const TOptional<FString> Namespace = FTextInspector::GetNamespace(Text);
        const TOptional<FString> PlainKey = FTextInspector::GetKey(Text);
        if (!Namespace.IsSet() || !PlainKey.IsSet()) return false;
        OutId = FTextId(FTextKey(Namespace.GetValue()), FTextKey(PlainKey.GetValue()));
        return true;
    }

    FString DTNSourceName(TSubclassOf<UFGItemDescriptor> Item)
    {
        const FText Text = UFGItemDescriptor::GetItemName(Item);
        const FString* Source = FTextInspector::GetSourceString(Text);
        return (Source ? *Source : Text.ToString()).TrimStartAndEnd();
    }
}

TArray<FDTTLanguageOption> FDTTItemNames::ListLanguages()
{
    TArray<FDTTLanguageOption> Out;
    Out.Add({ DefaultLanguage(), TEXT("English") });

    // The options menu's own list, so the dropdown offers exactly what the game's does.
    if (const UDataTable* Table = UFGLocalizationSettings::GetOptionsMenuLanugagesDataTable().LoadSynchronous())
    {
        if (Table->GetRowStruct() && Table->GetRowStruct()->IsChildOf(FLocalizationEntry::StaticStruct()))
        {
            for (const TPair<FName, uint8*>& Row : Table->GetRowMap())
            {
                const FLocalizationEntry* Entry = reinterpret_cast<const FLocalizationEntry*>(Row.Value);
                const FString Code = DTNCanonicalCode(Row.Key.ToString());
                if (!Entry || Code == DefaultLanguage() || !HasLanguage(Code)) continue;
                if (!UFGLocalizationSettings::ShouldShowInCurrentConfig(*Entry)) continue;
                const FString Label = Entry->NativeName.IsEmpty() || Entry->NativeName == Entry->EnglishName
                    ? Entry->EnglishName
                    : FString::Printf(TEXT("%s (%s)"), *Entry->NativeName, *Entry->EnglishName);
                Out.Add({ Code, Label.IsEmpty() ? Code : Label });
            }
        }
    }
    if (Out.Num() > 1) return Out;

    // No table (or an unreadable one): whatever cultures the game's localization data covers.
    for (const FString& Code : FTextLocalizationManager::Get().GetLocalizedCultureNames(ELocalizationLoadFlags::Game))
    {
        if (Code == DefaultLanguage() || !HasLanguage(Code)) continue;
        const FCulturePtr Culture = FInternationalization::Get().GetCulture(Code);
        Out.Add({ Code, Culture.IsValid() ? Culture->GetNativeName() : Code });
    }
    return Out;
}

bool FDTTItemNames::HasLanguage(const FString& Code)
{
    if (Code == DefaultLanguage()) return true;
    if (Code.IsEmpty() || Code.Contains(TEXT("/")) || Code.Contains(TEXT("\\")) || Code.Contains(TEXT(".."))) return false;
    return DTNLocresFiles(Code).Num() > 0;
}

FString FDTTItemNames::NormalizeLanguage(const FString& Code)
{
    const FString Clean = DTNCanonicalCode(Code.TrimStartAndEnd());
    return HasLanguage(Clean) ? Clean : FString(DefaultLanguage());
}

const TArray<TSubclassOf<UFGItemDescriptor>>& FDTTItemNames::AllItems(UObject* WorldContext)
{
    if (!bItemsCollected)
    {
        // Fluids included: fluid trucks and fluid truck stations exist.
        UFGBlueprintFunctionLibrary::GetAllDescriptorsSorted(WorldContext, Items,
            /*includeFluids*/ true, /*includeBuildings*/ false, /*onlyUnlocked*/ false);
        Items.RemoveAll([](const TSubclassOf<UFGItemDescriptor>& Item) { return !Item; });
        bItemsCollected = Items.Num() > 0;
    }
    return Items;
}

const FDTTItemNames::FTable& FDTTItemNames::TableFor(const FString& Code, UObject* WorldContext)
{
    FTable& Table = Tables.FindOrAdd(Code);
    if (Table.bBuilt) return Table;
    Table.bBuilt = true;

    const double Started = FPlatformTime::Seconds();
    const TArray<TSubclassOf<UFGItemDescriptor>>& All = AllItems(WorldContext);

    if (Code == DefaultLanguage())
    {
        for (const TSubclassOf<UFGItemDescriptor>& Item : All) Table.Names.Add(Item, DTNSourceName(Item));
        return Table;
    }

    // The culture's own files, read straight from the pak; nothing about the process changes.
    FTextLocalizationResource Resource;
    for (const FString& File : DTNLocresFiles(Code)) Resource.LoadFromFile(File, 0);

    int32 Missing = 0;
    for (const TSubclassOf<UFGItemDescriptor>& Item : All)
    {
        const FText Text = UFGItemDescriptor::GetItemName(Item);
        const FString Source = DTNSourceName(Item);
        FString Name = Source;

        FTextId Id;
        const FTextLocalizationResource::FEntry* Entry = DTNTextId(Text, Id) ? Resource.Entries.Find(Id) : nullptr;
        // Same check the engine makes: a translation of an older source string is stale, not a name.
        if (Entry && Entry->LocalizedString.IsValid() &&
            Entry->SourceStringHash == FTextLocalizationResource::HashString(Source))
        {
            Name = Entry->LocalizedString->TrimStartAndEnd();
        }
        else
        {
            ++Missing;
        }
        Table.Names.Add(Item, Name.IsEmpty() ? Source : Name);
    }

    UE_LOG(LogDTTItemNames, Display, TEXT("ITEMNAMES_TABLE Code=%s Items=%d Missing=%d Entries=%d Ms=%.1f"),
        *Code, All.Num(), Missing, Resource.Entries.Num(), (FPlatformTime::Seconds() - Started) * 1000.0);
    return Table;
}

FString FDTTItemNames::NameOf(TSubclassOf<UFGItemDescriptor> Item, const FString& Code, UObject* WorldContext)
{
    if (!Item) return FString();
    const FTable& Table = TableFor(NormalizeLanguage(Code), WorldContext);
    if (const FString* Name = Table.Names.Find(Item)) return *Name;
    // An item the descriptor walk did not list (a mod's, unlocked later): the game's own answer.
    return UFGItemDescriptor::GetItemName(Item).ToString().TrimStartAndEnd();
}

void FDTTItemNames::MergeIntoResolver(const FString& Code, UObject* WorldContext)
{
    bool bAlready = false;
    MergedCodes.Add(Code, &bAlready);
    if (bAlready) return;
    for (const TPair<TSubclassOf<UFGItemDescriptor>, FString>& Pair : TableFor(Code, WorldContext).Names)
    {
        if (Pair.Value.IsEmpty()) continue;
        ClassByName.FindOrAdd(Pair.Value.ToLower(), Pair.Key);
    }
}

TSubclassOf<UFGItemDescriptor> FDTTItemNames::Resolve(const FString& Name, const FString& PreferredCode, UObject* WorldContext)
{
    const FString Key = Name.TrimStartAndEnd().ToLower();
    if (Key.IsEmpty()) return nullptr;

    // The cheap languages first: whoever wrote the tag most likely used one of these.
    MergeIntoResolver(DefaultLanguage(), WorldContext);
    MergeIntoResolver(NormalizeLanguage(PreferredCode), WorldContext);
    MergeIntoResolver(NormalizeLanguage(FInternationalization::Get().GetCurrentLanguage()->GetName()), WorldContext);
    if (const TSubclassOf<UFGItemDescriptor>* Found = ClassByName.Find(Key)) return *Found;

    // Then, once, everything the game ships: a word that is a game name in any language is not the
    // player's own, and only the full set can say so.
    if (!bAllLanguagesMerged)
    {
        bAllLanguagesMerged = true;
        const double Started = FPlatformTime::Seconds();
        for (const FString& Code : FTextLocalizationManager::Get().GetLocalizedCultureNames(ELocalizationLoadFlags::Game))
        {
            if (HasLanguage(Code)) MergeIntoResolver(Code, WorldContext);
        }
        UE_LOG(LogDTTItemNames, Display, TEXT("ITEMNAMES_ALL Languages=%d Names=%d Ms=%.1f"),
            MergedCodes.Num(), ClassByName.Num(), (FPlatformTime::Seconds() - Started) * 1000.0);
        if (const TSubclassOf<UFGItemDescriptor>* Found = ClassByName.Find(Key)) return *Found;
    }
    return nullptr;
}
