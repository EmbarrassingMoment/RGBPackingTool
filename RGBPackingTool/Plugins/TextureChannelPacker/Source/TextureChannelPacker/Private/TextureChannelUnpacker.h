#pragma once

#include "CoreMinimal.h"
#include "Input/Reply.h"
#include "UObject/StrongObjectPtr.h"
#include "TextureChannelPacker.h"

class UTexture2D;
class SWidget;
struct FSlateBrush;
template <typename OptionType> class SComboBox;

/**
 * @class FTextureChannelUnpacker
 * @brief Implements the "Unpack" tab: splits a packed RGBA texture into per-channel grayscale assets.
 *
 * Owns all Unpack tab UI state. The module creates one instance at startup and keeps it alive
 * until shutdown, so widget lambdas may safely capture the raw `this` pointer (the same lifetime
 * contract the module uses for its own Pack tab lambdas).
 *
 * Feature summary:
 * - Selecting a source texture immediately previews its R/G/B/A channels as a 2x2 grayscale grid.
 * - Channels whose pixels all share one value (e.g. an unused alpha that is all 255) are flagged
 *   as "possibly unused" and excluded from export by default.
 * - Per-channel export toggles choose which channels become assets.
 * - Output names follow <Base><ChannelSuffix>, where the suffix comes from the selected preset
 *   (e.g. "_AO"/"_Roughness"/"_Metallic" for ORM) and the base name is derived from the source
 *   texture name with known packed suffixes (e.g. "_ORM") stripped.
 * - Extracted assets are single-channel TSF_G8 textures with TC_Grayscale compression and sRGB off.
 */
class FTextureChannelUnpacker
{
public:
    /**
     * @brief Wires the unpacker to the module-owned preset list.
     *
     * @param InPresets Preset list owned by the module. Must outlive this object.
     * @param InDefaultPreset Preset initially selected for suffix naming (typically ORM).
     */
    void Initialize(const TArray<TSharedPtr<FChannelPackerPreset>>* InPresets, TSharedPtr<FChannelPackerPreset> InDefaultPreset);

    /** Builds the Slate content for the Unpack tab. */
    TSharedRef<SWidget> CreateContent();

    /**
     * @brief Refreshes the preset dropdown after the module saves or deletes a preset.
     *
     * If the currently selected preset was deleted, falls back to the first preset in the list.
     */
    void OnPresetListChanged();

    /** Releases preview textures and brushes. Called on module shutdown. */
    void ReleaseResources();

private:
    /**
     * @brief Handles the 'Unpack Textures' button click event.
     *
     * Validates inputs (source texture, selected channels, base name), confirms overwrites,
     * then extracts each selected channel into its own grayscale texture asset.
     *
     * @return FReply::Handled() to indicate the event was consumed.
     */
    FReply OnExtractClicked();

    /** Called when the source texture picker changes. Updates naming and rebuilds the preview. */
    void OnSourceTextureChanged(UTexture2D* NewTexture);

    /**
     * @brief Re-reads the source texture and rebuilds the per-channel preview grid.
     *
     * Runs uniform-value detection on the full-resolution raw data (cheap: early-exits on the
     * first differing pixel), auto-unchecking channels that appear unused, then builds four
     * grayscale preview textures at a small capped resolution. Must be called on the Game Thread.
     */
    void UpdatePreview();

    /**
     * @brief Derives the output base name from the source texture name.
     *
     * Strips a known packed suffix (any preset's FileNameSuffix, e.g. "_ORM"), enforces the
     * "T_" prefix, and trims trailing underscores. Does nothing once the user has manually
     * edited the base name.
     */
    void AutoGenerateBaseName();

    /** Returns the per-channel filename suffix from the current preset (falls back to _R/_G/_B/_A). */
    FString GetChannelSuffix(int32 ChannelIndex) const;

    /** Returns the asset name for a channel: <Base><ChannelSuffix>. */
    FString GetChannelAssetName(int32 ChannelIndex) const;

    /** Returns the full package name (path + asset name) for a channel. */
    FString GetChannelPackageName(int32 ChannelIndex) const;

    /** Returns the header label for a channel cell, e.g. "R" or "R → AO" (meaning from the suffix). */
    FText GetChannelHeaderText(int32 ChannelIndex) const;

    /** Applies the given preset: updates naming suffixes and refreshes the base name. */
    void ApplyPreset(TSharedPtr<FChannelPackerPreset> Preset);

    /** Creates one cell of the 2x2 channel preview grid (checkbox + badge + image + output name). */
    TSharedRef<SWidget> CreateChannelCell(int32 ChannelIndex);

    // ========== Source ==========

    /** The packed texture to split into per-channel assets. */
    TWeakObjectPtr<UTexture2D> SourceTexture;

    // ========== Channel State (indexed R=0, G=1, B=2, A=3) ==========

    /** Whether each channel is exported as an asset. Auto-managed by uniform detection, user-overridable. */
    bool bExportChannel[4] = { true, true, true, true };

    /** True if every pixel of the channel holds the same value (likely unused). */
    bool bChannelUniform[4] = { false, false, false, false };

    /** The uniform value when bChannelUniform is true (shown in the badge). */
    uint8 ChannelUniformValue[4] = { 0, 0, 0, 0 };

    /** Transient grayscale preview textures, one per channel. Kept alive via TStrongObjectPtr. */
    TStrongObjectPtr<UTexture2D> ChannelPreviewTextures[4];

    /** Slate brushes pointing at the per-channel preview textures. */
    TSharedPtr<FSlateBrush> ChannelBrushes[4];

    // ========== Preview ==========

    /** True once a preview has been generated for the current source texture. */
    bool bPreviewValid = false;

    /** Display dimensions (in Slate units) of each preview cell image. */
    int32 PreviewDisplayWidth = 0;
    int32 PreviewDisplayHeight = 0;

    // ========== Output Settings ==========

    /** The package path where the extracted textures will be saved (e.g., "/Game/Textures/") */
    FString OutputPackagePath = TEXT("/Game/");

    /** Base filename; the per-channel suffix is appended (e.g., "T_Rock" -> "T_Rock_AO"). */
    FString BaseFileName = TEXT("T_Unpacked");

    /**
     * Flag to track whether the user has manually edited the base name.
     * When true, auto-generation is disabled to preserve user input.
     */
    bool bBaseNameManuallyEdited = false;

    // ========== Presets ==========

    /** Module-owned preset list (shared with the Pack tab). Never owned by this class. */
    const TArray<TSharedPtr<FChannelPackerPreset>>* Presets = nullptr;

    /** The preset providing the per-channel output suffixes. Independent of the Pack tab selection. */
    TSharedPtr<FChannelPackerPreset> CurrentPreset;

    /** Weak reference to the preset combo box for refreshing options. */
    TSharedPtr<SComboBox<TSharedPtr<FChannelPackerPreset>>> PresetComboBox;
};
