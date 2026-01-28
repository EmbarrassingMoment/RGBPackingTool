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

    /**
     * @brief Validates that input textures are unique and compatible.
     *
     * Checks for duplicate texture assignments across channels and ensures
     * that all provided textures are valid and accessible.
     *
     * @param OutErrorMessage If validation fails, contains a localized error message for the user.
     * @return True if all inputs are valid, false otherwise.
     */
    bool ValidateInputTextures(FText& OutErrorMessage) const;

    /**
     * @brief Checks if a given value is a power of two.
     *
     * Power-of-two resolutions are often more efficient for GPU processing,
     * though this plugin supports any resolution from 1 to 8192.
     *
     * @param Value The resolution value to check.
     * @return True if the value is a power of two (1, 2, 4, 8, 16, ..., 8192), false otherwise.
     */
    bool IsPowerOfTwo(int32 Value) const;

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

    // ========== Input Textures ==========

    /** Texture to be packed into the Red channel of the output (e.g., Ambient Occlusion) */
    TWeakObjectPtr<UTexture2D> InputTextureR;

    /** Texture to be packed into the Green channel of the output (e.g., Roughness) */
    TWeakObjectPtr<UTexture2D> InputTextureG;

    /** Texture to be packed into the Blue channel of the output (e.g., Metallic) */
    TWeakObjectPtr<UTexture2D> InputTextureB;

    /**
     * Texture to be packed into the Alpha channel of the output (optional).
     * If not provided, the Alpha channel defaults to white (255 / fully opaque).
     */
    TWeakObjectPtr<UTexture2D> InputTextureA;

    // ========== Output Settings ==========

    /** The package path where the generated texture will be saved (e.g., "/Game/Textures/") */
    FString OutputPackagePath = "/Game/";

    /** The filename for the generated texture asset (without extension) */
    FString OutputFileName = "T_Packed_Texture";

    /** Target resolution for the output texture (width and height, in pixels). Valid range: 1-8192. */
    int32 TargetResolution = 2048;

    // ========== Compression Settings ==========

    /** Available compression options for the dropdown menu ("Masks", "Grayscale", "Default") */
    TArray<TSharedPtr<FString>> CompressionOptions;

    /** The currently selected compression option from the dropdown */
    TSharedPtr<FString> CurrentCompressionOption;

    // ========== Internal State ==========

    /**
     * Flag to track whether the user has manually edited the output filename.
     * When true, auto-generation of filenames is disabled to preserve user input.
     */
    bool bFileNameManuallyEdited = false;
};
