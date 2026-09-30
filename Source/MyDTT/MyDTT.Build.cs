using UnrealBuildTool;

public class MyDTT : ModuleRules
{
    public MyDTT(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "InputCore",
            // The window's hotkeys: a mapping context and actions made at runtime.
            "EnhancedInput",
            "FactoryGame",
            "SML"
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "UMG",
            "Slate",
            "SlateCore",
            // Cargo paint: the dev command that bakes item colours reads every descriptor from the
            // asset registry and renders its icon into a render target. Client-side only, but the
            // module has to link either way.
            "AssetRegistry",
            "RenderCore"
        });
    }
}
