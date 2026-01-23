#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Input/Reply.h"
#include "Engine/Texture.h"

class SDockTab;
class FSpawnTabArgs;
class UTexture2D;

/**
 * @class FTextureChannelPackerModule
 * @brief The main module class for the Texture Channel Packer plugin.
 *
 * This class handles the initialization and shutdown of the module, manages the UI dock tab,
 * and coordinates the texture packing process. It serves as the central hub for user interaction
 * (selecting textures, settings) and executing the packing logic.
 */
class FTextureChannelPackerModule : public IModuleInterface
{
public:
    /**
     * @brief Called right after the module DLL has been loaded and the module object has been created.
     *
     * Registers the Nomad Tab spawner and extends the editor menu to include the tool.
     */
    virtual void StartupModule() override;

    /**
     * @brief Called before the module is unloaded, right before the module object is destroyed.
     *
     * Unregisters the tab spawner and cleans up menu extensions.
     */
    virtual void ShutdownModule() override;

private:
    /**
     * @brief Spawns the main dock tab for the plugin.
     *
     * @param SpawnTabArgs Arguments for spawning the tab.
     * @return A reference to the newly created SDockTab containing the plugin UI.
     */
    TSharedRef<SDockTab> OnSpawnPluginTab(const FSpawnTabArgs& SpawnTabArgs);

    /** Input textures for each channel */
    TWeakObjectPtr<UTexture2D> InputTextureR;
    TWeakObjectPtr<UTexture2D> InputTextureG;
    TWeakObjectPtr<UTexture2D> InputTextureB;
    TWeakObjectPtr<UTexture2D> InputTextureA;

    /** Output Settings */
    FString OutputPackagePath = "/Game/";
    FString OutputFileName = "T_Packed_Texture";
    int32 TargetResolution = 2048;

    /**
     * @brief Handles the 'Generate Texture' button click event.
     *
     * Validates inputs (textures, resolution, filename) and triggers the texture creation process.
     *
     * @return FReply::Handled() to indicate the event was consumed.
     */
    FReply OnGenerateClicked();

    /**
     * @brief Automatically generates a suggested output file name based on the input textures.
     *
     * Logic checks for common prefixes among inputs or uses the first available input name.
     * It appends suffixes (e.g., _ORM) and ensures the name follows conventions (e.g., T_ prefix).
     * This function does nothing if the user has manually edited the filename.
     */
    void AutoGenerateFileName();

    /**
     * @brief Creates the packed texture asset.
     *
     * Executes the packing workflow: extracting source data, processing it in parallel,
     * and writing the final pixels to a new UTexture2D asset.
     *
     * @param PackageName The full package path and name for the new asset.
     * @param Resolution The target resolution (width and height) for the square texture.
     */
    void CreateTexture(const FString& PackageName, int32 Resolution);

    /**
     * @brief Displays a notification toast in the editor.
     *
     * @param Message The text message to display.
     * @param bSuccess If true, shows a success icon; otherwise, shows an error icon.
     */
    void ShowNotification(const FText& Message, bool bSuccess);

    /** Compression Options */
    TArray<TSharedPtr<FString>> CompressionOptions;
    TSharedPtr<FString> CurrentCompressionOption;

    /**
     * @brief Converts the currently selected compression option string to the corresponding Unreal Engine enum.
     *
     * @return The TextureCompressionSettings enum value (e.g., TC_Masks, TC_Grayscale, TC_Default).
     */
    TextureCompressionSettings GetSelectedCompressionSettings() const;

    /**
     * @brief Creates a UI widget for a single texture input channel.
     *
     * Includes a label, an optional tooltip, and an object picker for UTexture2D.
     *
     * @param LabelText The display name for the channel (e.g., "Red Channel").
     * @param TargetTexturePtr A reference to the member variable that will hold the selected texture.
     * @param TooltipText Optional tooltip text describing the channel's usage.
     * @return A shared reference to the created widget.
     */
    TSharedRef<SWidget> CreateChannelInputSlot(const FText& LabelText, TWeakObjectPtr<UTexture2D>& TargetTexturePtr, const FText& TooltipText = FText::GetEmpty());

    /** Flag to track if the user has manually edited the output filename */
    bool bFileNameManuallyEdited = false;
};
