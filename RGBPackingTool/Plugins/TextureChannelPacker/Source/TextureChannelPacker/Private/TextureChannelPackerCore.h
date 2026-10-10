#pragma once

#include "CoreMinimal.h"
#include "TextureChannelPacker.h"
#include "TextureChannelPackerTypes.h"

/**
 * UI-independent Pack / Unpack pipeline.
 *
 * Everything the tool does to produce assets lives here, so the Slate UI, the Blueprint /
 * Python function library and the commandlet all run the same code. Nothing in this namespace
 * opens a dialog or shows a notification: problems are returned in FChannelPackerResult
 * (Status, ErrorCode, Warnings) and the caller decides how to present them. The only optional
 * UI is the cancellable progress dialog the editor tool asks for via FExecutionOptions.
 *
 * All functions must be called on the Game Thread.
 */
namespace TextureChannelPackerCore
{
    /** How a request is run. Defaults suit headless callers. */
    struct FExecutionOptions
    {
        /** Show a cancellable progress dialog (editor tool only). */
        bool bShowProgressDialog = false;
    };

    /**
     * @brief Packs up to four textures into one RGBA (BGRA8) texture asset.
     *
     * Validates the request, resolves the output size and name, honours the overwrite policy,
     * then extracts/resizes the channels in parallel and writes the asset (and saves it if
     * Request.bSave). Inputs that cannot be read are filled with the slot default and reported
     * as warnings, matching the editor tool.
     */
    FChannelPackerResult Pack(const FChannelPackerPackRequest& Request, const FExecutionOptions& Options = FExecutionOptions());

    /**
     * @brief Splits a texture into one TSF_G8 / TC_Grayscale / sRGB-off asset per channel at source resolution.
     *
     * With Request.bSkipUniformChannels, requested channels whose pixels all share one value are
     * left out (and listed in SkippedUniformChannels), matching the Unpack tab's default selection.
     */
    FChannelPackerResult Unpack(const FChannelPackerUnpackRequest& Request, const FExecutionOptions& Options = FExecutionOptions());

    // ========== Naming ==========

    /**
     * @brief Joins a content folder and an asset name into a long package name.
     *
     * "/Game/Folder" or "/Game/Folder/" + "T_X" -> "/Game/Folder/T_X". Nothing is trimmed or
     * collapsed, so the result is validated exactly as typed. The editor tool uses this for its
     * overwrite check, so the asset it checks is always the asset the core writes.
     */
    FString MakePackageName(const FString& OutputPath, const FString& AssetName);

    /**
     * @brief The tool's auto naming for packed outputs.
     *
     * Uses the longest common prefix of the input names (if at least 3 characters, otherwise the
     * first name), enforces the "T_" prefix, trims trailing underscores and appends Suffix.
     * Returns an empty string if InputNames is empty.
     */
    FString MakePackedAssetName(const TArray<FString>& InputNames, const FString& Suffix);

    /**
     * @brief The tool's auto naming for unpack outputs (before the per-channel suffix).
     *
     * Strips the longest matching known packed suffix (e.g. "_ORM"), enforces the "T_" prefix
     * and trims trailing underscores.
     */
    FString MakeUnpackBaseName(const FString& SourceName, const TArray<FString>& KnownPackedSuffixes);

    // ========== Compression ==========

    /** The compression options offered by the tool, in UI order ("Masks", "Grayscale", "Default"). */
    TArray<FCompressionOption> GetCompressionOptions();

    /** Looks up a compression option by its internal name (case-insensitive). */
    bool FindCompressionSetting(const FString& Name, TextureCompressionSettings& OutSetting);

    // ========== Presets ==========

    /** The built-in presets, in UI order: Custom (sentinel), ORM, MRA. */
    TArray<FChannelPackerPreset> GetBuiltInPresets();

    /** User presets stored under Saved/TextureChannelPacker/Presets. */
    TArray<FChannelPackerPreset> LoadUserPresets();

    /** Writes a user preset to disk (overwrites a preset with the same name). */
    bool SaveUserPreset(const FChannelPackerPreset& Preset);

    /** Deletes a user preset from disk. */
    bool DeleteUserPreset(const FString& PresetName);

    /** Built-in followed by user presets. */
    TArray<FChannelPackerPreset> GetAllPresets();

    /** Finds a preset by name (case-insensitive). Built-in presets win over user presets with the same name. */
    bool FindPreset(const FString& PresetName, FChannelPackerPreset& OutPreset);

    /** Every non-empty FileNameSuffix across all presets, longest first (used to derive unpack base names). */
    TArray<FString> GetKnownPackedSuffixes();

    /** Applies a preset's defaults (source channels, invert flags, compression, filename suffix) to a pack request. */
    void ApplyPresetToPackRequest(const FChannelPackerPreset& Preset, FChannelPackerPackRequest& Request);

    /** Applies a preset's per-channel unpack suffixes to an unpack request. */
    void ApplyPresetToUnpackRequest(const FChannelPackerPreset& Preset, FChannelPackerUnpackRequest& Request);

    // ========== Misc ==========

    /** "Succeeded", "Skipped", "DryRun", "Failed" or "Cancelled". */
    FString StatusToString(EChannelPackerStatus Status);
}
