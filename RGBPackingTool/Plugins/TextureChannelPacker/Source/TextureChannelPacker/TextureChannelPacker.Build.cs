using UnrealBuildTool;

public class TextureChannelPacker : ModuleRules
{
    public TextureChannelPacker(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicIncludePaths.AddRange(
            new string[] {
                // ... add public include paths required here ...
            }
        );

        PrivateIncludePaths.AddRange(
            new string[] {
                // ... add other private include paths required here ...
            }
        );

        PublicDependencyModuleNames.AddRange(
            new string[]
            {
                "Core",
                // Public headers declare reflected types (USTRUCT/UCLASS) and a Blueprint function library.
                "CoreUObject",
                "Engine",
                // ... add other public dependencies that you statically link with here ...
            }
        );

        PrivateDependencyModuleNames.AddRange(
            new string[]
            {
                "Slate",
                "SlateCore",
                "InputCore",
                "UnrealEd",
                "ToolMenus",
                "PropertyEditor",
                "ImageCore",
                "RenderCore",
                "AssetRegistry",
                "ContentBrowser",
                "Json",
                "JsonUtilities"
                // ... add private dependencies that you statically link with here ...
            }
        );

        DynamicallyLoadedModuleNames.AddRange(
            new string[]
            {
                // ... add any modules that your module loads dynamically here ...
            }
        );
    }
}
