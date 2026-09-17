#include "DTTCargoPaint.h"

#include "Resources/FGItemDescriptor.h"
#include "Resources/FGResourceDescriptor.h"
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

    /**
     * Saturated enough to read as a colour on a truck, bright enough to see in the rain. The
     * clamps are in linear space, where a value of 0.12 is already a mid grey on screen.
     */
    FLinearColor DTPReadable(const FLinearColor& In)
    {
        FLinearColor HSV = In.LinearRGBToHSV();
        HSV.G = FMath::Clamp(HSV.G, 0.35f, 1.0f);   // saturation
        HSV.B = FMath::Clamp(HSV.B, 0.12f, 0.85f);  // value
        FLinearColor Out = HSV.HSVToLinearRGB();
        Out.A = 1.0f;
        return Out;
    }

    /** The trim for an item that has only one colour: the same colour, darker. */
    FLinearColor DTPTrim(const FLinearColor& In)
    {
        return FLinearColor(In.R * 0.35f, In.G * 0.35f, In.B * 0.35f, 1.0f);
    }
}

const TMap<FName, FDTTCargoColors>& FDTTCargoPaint::BakedTable()
{
    static TMap<FName, FDTTCargoColors> Table;
    static bool bBuilt = false;
    if (!bBuilt)
    {
        bBuilt = true;
        // One line per item in the generated table, which may legitimately be empty (a build made
        // before the first bake) - hence a macro rather than an array initialiser.
#define DTT_BAKED_COLOR(ClassName, R, G, B, R2, G2, B2)         { FDTTCargoColors Colors; Colors.Primary = FLinearColor(R, G, B, 1.0f);           Colors.Secondary = FLinearColor(R2, G2, B2, 1.0f); Table.Add(FName(TEXT(ClassName)), Colors); }
#include "DTTCargoPaintTable.inc"
#undef DTT_BAKED_COLOR
        UE_LOG(LogDTTCargoPaint, Log, TEXT("CARGO_PAINT_TABLE Entries=%d"), Table.Num());
    }
    return Table;
}

FDTTCargoColors FDTTCargoPaint::ColorsFor(TSubclassOf<UFGItemDescriptor> Item)
{
    FDTTCargoColors Colors;
    if (!Item) return Colors;

    // Fluids and gases carry their colour on the descriptor, and it is the colour the player sees
    // in every pipe and gauge - no reason to invent another one. They have only the one, so the
    // trim is that colour darkened.
    const EResourceForm Form = UFGItemDescriptor::GetForm(Item);
    if (Form == EResourceForm::RF_LIQUID || Form == EResourceForm::RF_GAS)
    {
        FLinearColor Fluid = Form == EResourceForm::RF_LIQUID
            ? UFGItemDescriptor::GetFluidColorLinear(Item)
            : UFGItemDescriptor::GetGasColorLinear(Item);
        // The alpha of those two is the fluid's transparency, not a colour channel.
        Fluid.A = 1.0f;
        if (!Fluid.Equals(FLinearColor::Black, 0.01f))
        {
            Colors.Primary = DTPReadable(Fluid);
            Colors.Secondary = DTPTrim(Colors.Primary);
            return Colors;
        }
    }

    const FName ClassName = Item->GetFName();
    if (const FDTTCargoColors* Baked = BakedTable().Find(ClassName))
    {
        return *Baked;
    }

    // Anything the table does not know - a modded item, or a vanilla one added after the bake -
    // still gets a colour, and the same one every session: the hue is the hash of the class name.
    // CRC rather than GetTypeHash, which is only promised to be stable within one process.
    const uint32 Hash = FCrc::StrCrc32(*ClassName.ToString());
    Colors.Primary = FLinearColor::MakeFromHSV8(static_cast<uint8>(Hash % 256), 190, 230);
    Colors.Secondary = DTPTrim(Colors.Primary);
    return Colors;
}

bool FDTTCargoPaint::IsAvailable()
{
    return DTPCustomSwatch() != nullptr;
}

FFactoryCustomizationData FDTTCargoPaint::MakePaint(const FFactoryCustomizationData& Base, const FDTTCargoColors& Colors)
{
    FFactoryCustomizationData Data = Base;
    Data.SwatchDesc = DTPCustomSwatch();
    Data.OverrideColorData.PrimaryColor = Colors.Primary;
    Data.OverrideColorData.SecondaryColor = Colors.Secondary;
    // Cached from the swatch by Initialize() and not replicated, but the server applies the paint
    // to its own copy straight away and reads this on the way.
    Data.ColorSlot = INDEX_CUSTOM_COLOR_SLOT;
    return Data;
}

bool FDTTCargoPaint::IsPainted(const FFactoryCustomizationData& Data, const FDTTCargoColors& Colors)
{
    return Data.SwatchDesc == DTPCustomSwatch()
        && Data.OverrideColorData.PrimaryColor.Equals(Colors.Primary, 0.01f)
        && Data.OverrideColorData.SecondaryColor.Equals(Colors.Secondary, 0.01f);
}

#if !UE_SERVER
namespace
{
    /**
     * The two colours of one icon.
     *
     * A flat mean is grey for almost every icon: most are a grey machine part with the telling
     * colour in a corner. So the pixels are put into twelve hue buckets weighted by saturation
     * squared, the heaviest bucket is the main colour, and the heaviest bucket at least a sixth of
     * a circle away from it - and carrying at least a sixth of the weight - is the trim. An item
     * that is really one colour has no such second bucket, and gets its own colour darkened.
     */
    bool DTPIconColors(UObject* WorldContext, UTextureRenderTarget2D* Target, UTexture2D* Icon, FDTTCargoColors& OutColors)
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
            // Translucent, not opaque: an opaque draw copies the texture's colour where the icon
            // is transparent too, and that colour is neither black nor the icon's - it is whatever
            // the artist left outside the shape. Blended over a transparent target, alpha is
            // coverage and says which pixels are the icon.
            Canvas->K2_DrawTexture(Icon, FVector2D::ZeroVector, Size, FVector2D::ZeroVector,
                FVector2D::UnitVector, FLinearColor::White, BLEND_Translucent);
        }
        UKismetRenderingLibrary::EndDrawCanvasToRenderTarget(WorldContext, Context);

        // Raw, not ReadRenderTarget: that one quantises to FColor, and reading those bytes as sRGB
        // (they are already linear) is what made the first bake come out near-black.
        TArray<FLinearColor> Pixels;
        if (!UKismetRenderingLibrary::ReadRenderTargetRaw(WorldContext, Target, Pixels, true) || Pixels.Num() == 0)
        {
            return false;
        }

        constexpr int32 BucketCount = 12;
        double SumR[BucketCount] = {}, SumG[BucketCount] = {}, SumB[BucketCount] = {}, Weight[BucketCount] = {};
        double TotalWeight = 0.0;
        for (const FLinearColor& Pixel : Pixels)
        {
            if (Pixel.A < 0.5f) continue;
            // The blend leaves the colour multiplied by coverage; undo it, or a soft edge reads as
            // a darker shade of the same hue and drags the average down.
            const FLinearColor Linear(Pixel.R / Pixel.A, Pixel.G / Pixel.A, Pixel.B / Pixel.A, 1.0f);
            const FLinearColor HSV = Linear.LinearRGBToHSV();
            // Near-black outlines say nothing about the item and are most of some icons.
            if (HSV.B < 0.08f) continue;
            const int32 Bucket = FMath::Clamp(
                static_cast<int32>(HSV.R / 360.0f * BucketCount), 0, BucketCount - 1);
            // Saturation squared, not saturation: copper ore is a cool grey rock with copper veins,
            // and a linear weight let the large grey body outvote the small orange one - the ore
            // came out grey-blue. The epsilon keeps a genuinely colourless item (concrete, steel)
            // averaging to its own grey instead of to nothing.
            const double PixelWeight = FMath::Square(static_cast<double>(HSV.G)) + 0.02;
            SumR[Bucket] += Linear.R * PixelWeight;
            SumG[Bucket] += Linear.G * PixelWeight;
            SumB[Bucket] += Linear.B * PixelWeight;
            Weight[Bucket] += PixelWeight;
            TotalWeight += PixelWeight;
        }
        if (TotalWeight <= 0.0) return false;

        auto BucketColor = [&](int32 Bucket)
        {
            return DTPReadable(FLinearColor(
                static_cast<float>(SumR[Bucket] / Weight[Bucket]),
                static_cast<float>(SumG[Bucket] / Weight[Bucket]),
                static_cast<float>(SumB[Bucket] / Weight[Bucket]), 1.0f));
        };

        int32 Main = 0;
        for (int32 Bucket = 1; Bucket < BucketCount; ++Bucket)
        {
            if (Weight[Bucket] > Weight[Main]) Main = Bucket;
        }
        OutColors.Primary = BucketColor(Main);

        int32 Trim = INDEX_NONE;
        for (int32 Bucket = 0; Bucket < BucketCount; ++Bucket)
        {
            const int32 Apart = FMath::Min(FMath::Abs(Bucket - Main), BucketCount - FMath::Abs(Bucket - Main));
            if (Apart < 2 || Weight[Bucket] < TotalWeight * 0.15) continue;
            if (Trim == INDEX_NONE || Weight[Bucket] > Weight[Trim]) Trim = Bucket;
        }
        OutColors.Secondary = Trim == INDEX_NONE ? DTPTrim(OutColors.Primary) : BucketColor(Trim);
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

        // 16f: eight bits of linear are coarse in the darks, which is most of an icon. 64 px
        // because the same draw is exported as a picture for the preview page.
        UTextureRenderTarget2D* Target = UKismetRenderingLibrary::CreateRenderTarget2D(
            World, 64, 64, RTF_RGBA16f, FLinearColor::Transparent, false, false);
        if (!IsValid(Target))
        {
            UE_LOG(LogDTTCargoPaint, Error, TEXT("DUMP_NO_RENDER_TARGET"));
            return;
        }

        const FString IconDir = FPaths::ProjectSavedDir() / TEXT("DTT") / TEXT("Icons");
        IFileManager::Get().MakeDirectory(*IconDir, true);

        TArray<FString> Lines;
        int32 Skipped = 0;
        for (UClass* Item : Items)
        {
            UTexture2D* Icon = UFGItemDescriptor::GetSmallIcon(Item);
            if (!IsValid(Icon)) Icon = UFGItemDescriptor::GetBigIcon(Item);
            FDTTCargoColors Colors;
            if (!IsValid(Icon) || !DTPIconColors(World, Target, Icon, Colors)) { ++Skipped; continue; }
            // The icon is already drawn in the target, so writing it out costs nothing and gives
            // the preview page the real picture to hold the computed colours against.
            UKismetRenderingLibrary::ExportRenderTarget(
                World, Target, IconDir, Item->GetName() + TEXT(".png"));
            // A resource also has the colour the game pings it with on the map. It is not used for
            // the paint - most items have none - but printing it next to the computed colour is
            // the one independent check that the icon was read the right way round.
            FString PingNote;
            if (Item->IsChildOf(UFGResourceDescriptor::StaticClass()))
            {
                const FLinearColor Ping = UFGResourceDescriptor::GetPingColor(Item);
                PingNote = FString::Printf(TEXT(" ping=%.3f/%.3f/%.3f"), Ping.R, Ping.G, Ping.B);
            }
            Lines.Add(FString::Printf(TEXT("%s %.4f %.4f %.4f %.4f %.4f %.4f  # %s%s"),
                *Item->GetName(),
                Colors.Primary.R, Colors.Primary.G, Colors.Primary.B,
                Colors.Secondary.R, Colors.Secondary.G, Colors.Secondary.B,
                *UFGItemDescriptor::GetItemName(Item).ToString(), *PingNote));
        }

        const FString Path = FPaths::ProjectSavedDir() / TEXT("DTT") / TEXT("ItemColors.txt");
        // UTF-8: the default is UTF-16, which every text tool downstream reads as mojibake.
        const bool bWritten = FFileHelper::SaveStringArrayToFile(
            Lines, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
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
