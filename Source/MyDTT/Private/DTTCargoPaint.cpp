#include "DTTCargoPaint.h"

#include "Resources/FGItemDescriptor.h"
#include "Misc/Crc.h"
#include "UObject/UnrealType.h"
#include "UObject/ConstructorHelpers.h"
#include "UObject/UObjectIterator.h"

#if !UE_SERVER
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/Canvas.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogDTTCargoPaint, Log, All);

namespace
{
    /**
     * The game's own "custom colour" swatch - what the paint gun writes when the player picks a
     * colour of their own instead of one of the slots. With it set, OverrideColorData is what the
     * vehicle wears; with a slot swatch, the slot's colour wins and the override is ignored.
     */
    const TCHAR* DTPCustomSwatchPath =
        TEXT("/Game/FactoryGame/Buildable/-Shared/Customization/Swatches/SwatchDesc_Custom.SwatchDesc_Custom_C");

    TSubclassOf<UFGFactoryCustomizationDescriptor_Swatch> DTPCustomSwatch()
    {
        // Resolved once. A failure is remembered as a failure: retrying every second on a server
        // that cooked the swatch out would be a load attempt per truck per poll.
        static bool bTried = false;
        static TSubclassOf<UFGFactoryCustomizationDescriptor_Swatch> Swatch = nullptr;
        if (!bTried)
        {
            bTried = true;
            Swatch = LoadClass<UFGFactoryCustomizationDescriptor_Swatch>(nullptr, DTPCustomSwatchPath);
            if (!Swatch)
            {
                UE_LOG(LogDTTCargoPaint, Warning,
                    TEXT("CARGO_PAINT_UNAVAILABLE The game's custom swatch did not load from '%s'. Painting is off."),
                    DTPCustomSwatchPath);
            }
        }
        return Swatch;
    }

    /** Saturated enough to read as a colour on a truck, bright enough to see in the rain. */
    FLinearColor DTPReadable(const FLinearColor& In)
    {
        FLinearColor HSV = In.LinearRGBToHSV();
        HSV.G = FMath::Clamp(HSV.G, 0.45f, 1.0f);   // saturation
        HSV.B = FMath::Clamp(HSV.B, 0.35f, 0.95f);  // value
        FLinearColor Out = HSV.HSVToLinearRGB();
        Out.A = 1.0f;
        return Out;
    }
}

const TMap<FName, FLinearColor>& FDTTCargoPaint::BakedTable()
{
    static TMap<FName, FLinearColor> Table;
    static bool bBuilt = false;
    if (!bBuilt)
    {
        bBuilt = true;
        // One line per item in the generated table, which may legitimately be empty (a build made
        // before the first bake) - hence a macro rather than an array initialiser.
#define DTT_BAKED_COLOR(ClassName, R, G, B) Table.Add(FName(TEXT(ClassName)), FLinearColor(R, G, B, 1.0f));
#include "DTTCargoPaintTable.inc"
#undef DTT_BAKED_COLOR
        UE_LOG(LogDTTCargoPaint, Log, TEXT("CARGO_PAINT_TABLE Entries=%d"), Table.Num());
    }
    return Table;
}

FLinearColor FDTTCargoPaint::ColorFor(TSubclassOf<UFGItemDescriptor> Item)
{
    if (!Item) return FLinearColor::White;

    // Fluids and gases carry their colour on the descriptor, and it is the colour the player sees
    // in every pipe and gauge - no reason to invent another one.
    const EResourceForm Form = UFGItemDescriptor::GetForm(Item);
    if (Form == EResourceForm::RF_LIQUID || Form == EResourceForm::RF_GAS)
    {
        FLinearColor Fluid = Form == EResourceForm::RF_LIQUID
            ? UFGItemDescriptor::GetFluidColorLinear(Item)
            : UFGItemDescriptor::GetGasColorLinear(Item);
        // The alpha of those two is the fluid's transparency, not a colour channel.
        Fluid.A = 1.0f;
        if (!Fluid.Equals(FLinearColor::Black, 0.01f)) return DTPReadable(Fluid);
    }

    const FName ClassName = Item->GetFName();
    if (const FLinearColor* Baked = BakedTable().Find(ClassName))
    {
        return *Baked;
    }

    // Anything the table does not know - a modded item, or a vanilla one added after the bake -
    // still gets a colour, and the same one every session: the hue is the hash of the class name.
    // CRC rather than GetTypeHash, which is only promised to be stable within one process.
    const uint32 Hash = FCrc::StrCrc32(*ClassName.ToString());
    return FLinearColor::MakeFromHSV8(static_cast<uint8>(Hash % 256), 190, 230);
}

bool FDTTCargoPaint::IsAvailable()
{
    return DTPCustomSwatch() != nullptr;
}

FFactoryCustomizationData FDTTCargoPaint::MakePaint(const FFactoryCustomizationData& Base, const FLinearColor& Primary)
{
    FFactoryCustomizationData Data = Base;
    Data.SwatchDesc = DTPCustomSwatch();
    Data.OverrideColorData.PrimaryColor = Primary;
    // The secondary is the trim; a darkened primary keeps the truck one colour instead of two.
    Data.OverrideColorData.SecondaryColor = FLinearColor(Primary.R * 0.35f, Primary.G * 0.35f, Primary.B * 0.35f, 1.0f);
    // Cached from the swatch by Initialize() and not replicated, but the server applies the paint
    // to its own copy straight away and reads this on the way.
    Data.ColorSlot = INDEX_CUSTOM_COLOR_SLOT;
    return Data;
}

bool FDTTCargoPaint::IsPainted(const FFactoryCustomizationData& Data, const FLinearColor& Primary)
{
    return Data.SwatchDesc == DTPCustomSwatch()
        && Data.OverrideColorData.PrimaryColor.Equals(Primary, 0.01f);
}

#if !UE_SERVER
namespace
{
    /**
     * The average colour of one icon, ignoring what is transparent and leaning on what is coloured:
     * most item icons are a grey machine part with the telling colour in a corner, so a flat mean
     * comes out grey for everything. Saturation is the weight, with a floor so a genuinely grey
     * icon (concrete, steel) still averages to its own grey rather than to nothing.
     */
    bool DTPIconColor(UObject* WorldContext, UTextureRenderTarget2D* Target, UTexture2D* Icon, FLinearColor& OutColor)
    {
        if (!IsValid(Target) || !IsValid(Icon)) return false;

        // A streamed-out icon draws as a blurred lowest mip, which would still give a colour - but
        // the wrong one for a small icon whose mip 0 is all that carries the detail.
        Icon->SetForceMipLevelsToBeResident(30.0f);
        Icon->WaitForStreaming();

        UKismetRenderingLibrary::ClearRenderTarget2D(WorldContext, Target, FLinearColor::Transparent);
        UCanvas* Canvas = nullptr;
        FVector2D Size = FVector2D::ZeroVector;
        FDrawToRenderTargetContext Context;
        UKismetRenderingLibrary::BeginDrawCanvasToRenderTarget(WorldContext, Target, Canvas, Size, Context);
        if (IsValid(Canvas))
        {
            Canvas->K2_DrawTexture(Icon, FVector2D::ZeroVector, Size, FVector2D::ZeroVector,
                FVector2D::UnitVector, FLinearColor::White, BLEND_Opaque);
        }
        UKismetRenderingLibrary::EndDrawCanvasToRenderTarget(WorldContext, Context);

        TArray<FColor> Pixels;
        if (!UKismetRenderingLibrary::ReadRenderTarget(WorldContext, Target, Pixels, true) || Pixels.Num() == 0)
        {
            return false;
        }

        double SumR = 0.0, SumG = 0.0, SumB = 0.0, SumWeight = 0.0;
        for (const FColor& Pixel : Pixels)
        {
            if (Pixel.A < 128) continue;
            const FLinearColor Linear = FLinearColor::FromSRGBColor(Pixel);
            const FLinearColor HSV = Linear.LinearRGBToHSV();
            // Near-black outlines say nothing about the item and are most of some icons.
            if (HSV.B < 0.08f) continue;
            const double Weight = 0.15 + HSV.G;
            SumR += Linear.R * Weight; SumG += Linear.G * Weight; SumB += Linear.B * Weight;
            SumWeight += Weight;
        }
        if (SumWeight <= 0.0) return false;

        OutColor = DTPReadable(FLinearColor(
            static_cast<float>(SumR / SumWeight),
            static_cast<float>(SumG / SumWeight),
            static_cast<float>(SumB / SumWeight), 1.0f));
        return true;
    }

    /** Every item descriptor the cook shipped, loaded: the asset registry knows them all by path. */
    void DTPCollectItemDescriptors(TArray<UClass*>& OutClasses)
    {
        TSet<UClass*> Seen;
        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* Class = *It;
            if (Class->IsChildOf(UFGItemDescriptor::StaticClass()) &&
                !Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
            {
                Seen.Add(Class);
            }
        }

        const IAssetRegistry& Registry = FAssetRegistryModule::GetRegistry();
        TArray<FAssetData> Assets;
        Registry.GetAssetsByPath(FName(TEXT("/Game/FactoryGame")), Assets, /*bRecursive*/ true);
        UE_LOG(LogDTTCargoPaint, Log, TEXT("DUMP_SCAN Assets=%d Loaded=%d"), Assets.Num(), Seen.Num());
        for (const FAssetData& Asset : Assets)
        {
            // Only the descriptor naming convention is loaded. Loading every asset under
            // /Game/FactoryGame to ask what it is would pull the whole game into memory.
            const FString AssetName = Asset.AssetName.ToString();
            if (!AssetName.StartsWith(TEXT("Desc_")) && !AssetName.StartsWith(TEXT("BP_ItemDescriptor"))) continue;
            FString ClassPath = Asset.GetObjectPathString();
            if (!ClassPath.EndsWith(TEXT("_C"))) ClassPath += TEXT("_C");
            if (UClass* Loaded = LoadClass<UFGItemDescriptor>(nullptr, *ClassPath))
            {
                if (!Loaded->HasAnyClassFlags(CLASS_Abstract)) Seen.Add(Loaded);
            }
        }

        OutClasses = Seen.Array();
        OutClasses.Sort([](const UClass& A, const UClass& B) { return A.GetName() < B.GetName(); });
    }

    void DTPDumpItemColors(const TArray<FString>& Args, UWorld* World)
    {
        if (!IsValid(World))
        {
            UE_LOG(LogDTTCargoPaint, Warning, TEXT("DUMP_NO_WORLD"));
            return;
        }

        TArray<UClass*> Items;
        DTPCollectItemDescriptors(Items);

        UTextureRenderTarget2D* Target = UKismetRenderingLibrary::CreateRenderTarget2D(
            World, 32, 32, RTF_RGBA8, FLinearColor::Transparent, false, false);
        if (!IsValid(Target))
        {
            UE_LOG(LogDTTCargoPaint, Error, TEXT("DUMP_NO_RENDER_TARGET"));
            return;
        }

        TArray<FString> Lines;
        int32 Skipped = 0;
        for (UClass* Item : Items)
        {
            UTexture2D* Icon = UFGItemDescriptor::GetSmallIcon(Item);
            if (!IsValid(Icon)) Icon = UFGItemDescriptor::GetBigIcon(Item);
            FLinearColor Color;
            if (!IsValid(Icon) || !DTPIconColor(World, Target, Icon, Color)) { ++Skipped; continue; }
            Lines.Add(FString::Printf(TEXT("%s %.4f %.4f %.4f  # %s"),
                *Item->GetName(), Color.R, Color.G, Color.B,
                *UFGItemDescriptor::GetItemName(Item).ToString()));
        }

        const FString Path = FPaths::ProjectSavedDir() / TEXT("DTT") / TEXT("ItemColors.txt");
        const bool bWritten = FFileHelper::SaveStringArrayToFile(Lines, *Path);
        UE_LOG(LogDTTCargoPaint, Display, TEXT("DUMP_DONE Items=%d Written=%d Skipped=%d File='%s' Ok=%s"),
            Items.Num(), Lines.Num(), Skipped, *Path, bWritten ? TEXT("true") : TEXT("false"));
    }

    // A dev command, on the client only: a dedicated server's cook keeps no texture pixels, which
    // is the whole reason the table this writes is baked into the mod instead of read at runtime.
    FAutoConsoleCommandWithWorldAndArgs DTPDumpItemColorsCommand(
        TEXT("DTT.DumpItemColors"),
        TEXT("Writes Saved/DTT/ItemColors.txt: one line per item descriptor with the average colour of its icon."),
        FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&DTPDumpItemColors));
}
#endif
