#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Input/Reply.h"
#include "Engine/Texture.h"
#include "Dom/JsonObject.h"
#include "UObject/StrongObjectPtr.h"

class SDockTab;
class FSpawnTabArgs;
class UTexture2D;
struct FSlateBrush;
template <typename OptionType> class SComboBox;

/**
 * @enum ESourceChannel
 * @brief Identifies which channel of an input texture to read from.
 *
 * Used to let users pick R/G/B/A from each input texture instead of always reading Red.
 * For single-channel source formats (G8, G16, R16F, R32F), the selection is ignored
 * and the only available value is used.
 */
enum class ESourceChannel : uint8
{
    Red = 0,
    Green = 1,
    Blue = 2,
    Alpha = 3
};

/**
 * @struct FCompressionOption
 * @brief Represents a texture compression configuration option.
 *
 * Encapsulates the internal identifier, the corresponding Unreal Engine compression setting,
 * and localized display names for UI presentation.
 */
struct FCompressionOption
{
    /** Internal identifier for comparison and logic (locale-independent, e.g., "Masks"). */
    FString InternalName;

    /** The Unreal Engine texture compression setting (enum). */
    TextureCompressionSettings CompressionSetting;

    /** Display name in English. */
    FString DisplayNameEn;

    /** Display name in Japanese. */
    FString DisplayNameJa;

    /**
     * @brief Returns the display name localized for the current editor language.
     * @return FText The localized display name.
     */
    FText GetDisplayName() const;
};

/**
 * @struct FChannelPackerPreset
 * @brief Represents a saved channel packing configuration preset.
 *
 * Stores channel label descriptions, filename suffix, compression defaults,
 * and invert flag defaults. Can be serialized to/from JSON for persistence.
 */
struct FChannelPackerPreset
{
    /** Display name for this preset (e.g., "ORM", "MRA") */
    FString PresetName;

    /** If true, this is a built-in preset that cannot be deleted by the user. */
    bool bIsBuiltIn = false;

    /** Channel label descriptions (English) */
    FString RedLabelEn;
    FString GreenLabelEn;
    FString BlueLabelEn;
    FString AlphaLabelEn;

    /** Channel label descriptions (Japanese) */
    FString RedLabelJa;
    FString GreenLabelJa;
    FString BlueLabelJa;
    FString AlphaLabelJa;

    /** Suffix appended to auto-generated filenames (e.g., "_ORM", "_MRA") */
    FString FileNameSuffix;

    /** Internal name of the default compression option (matches FCompressionOption::InternalName) */
    FString DefaultCompressionName;

    /** Default invert flags per channel */
    bool bDefaultInvertR = false;
    bool bDefaultInvertG = false;
    bool bDefaultInvertB = false;
    bool bDefaultInvertA = false;

    /** Default source channel selection per output slot (which channel of the input texture to read). */
    ESourceChannel DefaultSourceChannelR = ESourceChannel::Red;
    ESourceChannel DefaultSourceChannelG = ESourceChannel::Red;
    ESourceChannel DefaultSourceChannelB = ESourceChannel::Red;
    ESourceChannel DefaultSourceChannelA = ESourceChannel::Red;

    /** Serializes this preset to a JSON object. */
    TSharedPtr<FJsonObject> ToJson() const;

    /** Deserializes a preset from a JSON object. */
    static FChannelPackerPreset FromJson(const TSharedPtr<FJsonObject>& JsonObject);

    /** Returns the localized display name for UI. */
    FText GetDisplayName() const;
};

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
     * @param Width The target width for the output texture.
     * @param Height The target height for the output texture.
     */
    void CreateTexture(const FString& PackageName, int32 Width, int32 Height);

    /**
     * @brief Displays a notification toast in the editor.
     *
     * @param Message The text message to display.
     * @param bSuccess If true, shows a success icon; otherwise, shows an error icon.
     */
    void ShowNotification(const FText& Message, bool bSuccess);

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
     * @param LabelText Attribute for the display name (supports dynamic updates via lambda).
     * @param TargetTexturePtr A reference to the member variable that will hold the selected texture.
     * @param bInvertFlag A reference to the boolean flag controlling channel inversion.
     * @param TooltipText Optional tooltip text describing the channel's usage.
     * @return A shared reference to the created widget.
     */
    TSharedRef<SWidget> CreateChannelInputSlot(const TAttribute<FText>& LabelText, TWeakObjectPtr<UTexture2D>& TargetTexturePtr, bool& bInvertFlag, ESourceChannel& SourceChannelRef, const FText& TooltipText = FText::GetEmpty());

    // ========== Input Textures ==========

    /** Flag to invert the Red channel input (255 - Value). */
    bool bInvertR = false;

    /** Flag to invert the Green channel input (255 - Value). */
    bool bInvertG = false;

    /** Flag to invert the Blue channel input (255 - Value). */
    bool bInvertB = false;

    /** Flag to invert the Alpha channel input (255 - Value). */
    bool bInvertA = false;

    /** Which channel of the input texture to read for each output slot. Defaults to Red. */
    ESourceChannel SourceChannelR = ESourceChannel::Red;
    ESourceChannel SourceChannelG = ESourceChannel::Red;
    ESourceChannel SourceChannelB = ESourceChannel::Red;
    ESourceChannel SourceChannelA = ESourceChannel::Red;

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

    /** Target width for the output texture (in pixels). Valid range: 1-16384. */
    int32 TargetWidth = 2048;

    /** Target height for the output texture (in pixels). Valid range: 1-16384. */
    int32 TargetHeight = 2048;

    // ========== Compression Settings ==========

    /** Available compression options for the dropdown menu ("Masks", "Grayscale", "Default") */
    TArray<TSharedPtr<FCompressionOption>> CompressionOptions;

    /** The currently selected compression option from the dropdown */
    TSharedPtr<FCompressionOption> CurrentCompressionOption;

    // ========== Internal State ==========

    /**
     * Flag to track whether the user has manually edited the output filename.
     * When true, auto-generation of filenames is disabled to preserve user input.
     */
    bool bFileNameManuallyEdited = false;

    // ========== Preset System ==========

    /** All available presets (built-in + user-created). */
    TArray<TSharedPtr<FChannelPackerPreset>> Presets;

    /** The currently active preset. */
    TSharedPtr<FChannelPackerPreset> CurrentPreset;

    /** The "Custom" sentinel preset (always first in the list). */
    TSharedPtr<FChannelPackerPreset> CustomPreset;

    /** Suffix for auto-generated filenames, updated when preset changes. */
    FString CurrentFileNameSuffix = TEXT("_ORM");

    /** Weak reference to the preset combo box for refreshing options. */
    TSharedPtr<SComboBox<TSharedPtr<FChannelPackerPreset>>> PresetComboBox;

    /** Shared option list (R/G/B/A) backing the per-slot source channel dropdowns. */
    TArray<TSharedPtr<ESourceChannel>> SourceChannelOptions;

    /** Returns the localized Red channel label from the current preset. */
    FText GetCurrentRedLabel() const;

    /** Returns the localized Green channel label from the current preset. */
    FText GetCurrentGreenLabel() const;

    /** Returns the localized Blue channel label from the current preset. */
    FText GetCurrentBlueLabel() const;

    /** Returns the localized Alpha channel label from the current preset. */
    FText GetCurrentAlphaLabel() const;

    /** Creates and registers the built-in presets (Custom, ORM, MRA). */
    void InitializeBuiltInPresets();

    /** Loads user-created presets from disk and appends them to the Presets array. */
    void LoadPresetsFromDisk();

    /** Applies the given preset: updates labels, invert flags, compression, and suffix. */
    void ApplyPreset(TSharedPtr<FChannelPackerPreset> Preset);

    /** Saves the current settings as a named user preset to disk. */
    void SaveCurrentAsPreset(const FString& Name);

    /** Deletes the currently selected user preset from disk and the Presets array. */
    void DeleteCurrentPreset();

    /** Switches to "Custom" preset if current settings differ from the active preset. */
    void MarkCustomIfChanged();

    // ========== Preview ==========

    /**
     * @brief Rebuilds the live preview thumbnail from the current inputs and settings.
     *
     * Runs the same extract/process pipeline as generation, but at a small capped resolution
     * (so it stays cheap even for 16K targets) and writes the result into a transient texture
     * displayed in the tool UI. The RGB channels are composited; Alpha is forced opaque so the
     * preview is not blended against the background. Must be called on the Game Thread.
     */
    void UpdatePreview();

    /** Transient texture backing the preview image. Kept alive via TStrongObjectPtr. */
    TStrongObjectPtr<UTexture2D> PreviewTexture;

    /** Slate brush that points at PreviewTexture for display in an SImage. */
    TSharedPtr<FSlateBrush> PreviewBrush;

    /** Display dimensions (in Slate units) of the current preview. */
    int32 PreviewDisplayWidth = 0;
    int32 PreviewDisplayHeight = 0;

    /** True once a preview has been generated at least once. */
    bool bPreviewValid = false;
};
