#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Input/Reply.h"
#include "Engine/Texture.h"

class SDockTab;
class FSpawnTabArgs;
class UTexture2D;

class FTextureChannelPackerModule : public IModuleInterface
{
public:
    /** IModuleInterface implementation */
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    /** Callback for spawning the plugin tab */
    TSharedRef<SDockTab> OnSpawnPluginTab(const FSpawnTabArgs& SpawnTabArgs);

    /** Input textures for each channel */
    TWeakObjectPtr<UTexture2D> InputTextureR;
    TWeakObjectPtr<UTexture2D> InputTextureG;
    TWeakObjectPtr<UTexture2D> InputTextureB;
    TWeakObjectPtr<UTexture2D> InputTextureA;

    /** Output Settings */
    FString OutputPackagePath = "/Game/Textures/Packed/";
    FString OutputFileName = "T_Packed_Texture";
    int32 TargetResolution = 2048;

    /** Callback for Generate button */
    FReply OnGenerateClicked();

    /** Helper function to create the texture asset */
    void CreateTexture(const FString& PackageName, int32 Resolution);

    /** Helper function to show notifications */
    void ShowNotification(const FText& Message, bool bSuccess);

    /** Auto-generates the output file name based on input textures */
    void AutoGenerateFileName();

    /** Compression Options */
    TArray<TSharedPtr<FString>> CompressionOptions;
    TSharedPtr<FString> CurrentCompressionOption;

    /** Helper to get the enum value */
    TextureCompressionSettings GetSelectedCompressionSettings() const;
};
