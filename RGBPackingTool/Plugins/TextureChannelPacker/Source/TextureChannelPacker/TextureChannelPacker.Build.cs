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
                // ... add other public dependencies that you statically link with here ...
            }
        );

        PrivateDependencyModuleNames.AddRange(
            new string[]
            {
                "CoreUObject",    // UObject システム
                "Engine",         // テクスチャとアセット管理
                "Slate",          // UI フレームワーク
                "SlateCore",      // UI コアコンポーネント
                "UnrealEd",       // エディター機能
                "ToolMenus",      // メニュー拡張
                "PropertyEditor", // プロパティピッカー
                "ImageCore",      // FImageUtils（リサイズ処理）
                "AssetRegistry",  // アセット登録
                "ContentBrowser"  // パスピッカー
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
