#pragma once

#include "CoreMinimal.h"
#include "UObject/SoftObjectPtr.h"
#include "TextureChannelPackerTypes.generated.h"

class UTexture2D;

/**
 * Request / result types for running Pack and Unpack without the editor UI.
 *
 * These are reflected so the same API is reachable from C++, Blueprint, Editor Python
 * (unreal.TextureChannelPackerLibrary), Remote Control and the TextureChannelPacker
 * commandlet. Everything here is locale-independent except FChannelPackerResult::Message,
 * which is localized for humans; machines should branch on Status and ErrorCode.
 */

/**
 * @enum EChannelPackerChannel
 * @brief A channel of a texture (R/G/B/A).
 *
 * The non-reflected code refers to this as ESourceChannel (see TextureChannelPacker.h). The
 * reflected name is prefixed so it cannot clash with another module's UENUM.
 */
UENUM(BlueprintType)
enum class EChannelPackerChannel : uint8
{
    Red = 0,
    Green = 1,
    Blue = 2,
    Alpha = 3
};

/** What to do when an output asset already exists. */
UENUM(BlueprintType)
enum class EChannelPackerOverwritePolicy : uint8
{
    /** Fail the request (ErrorCode "ErrorAssetExists") and write nothing. */
    Fail,
    /** Replace the existing asset(s). */
    Overwrite,
    /** Write nothing and report the request as Skipped. Useful for re-runnable batch jobs. */
    Skip
};

/** Outcome of a Pack or Unpack request. */
UENUM(BlueprintType)
enum class EChannelPackerStatus : uint8
{
    /** The output asset(s) were created (and saved, if requested). */
    Succeeded,
    /** Nothing was written because an output already exists and OverwritePolicy is Skip. */
    Skipped,
    /** The request is valid; nothing was written because bDryRun was set. */
    DryRun,
    /** The request failed; see ErrorCode and Message. */
    Failed,
    /** The user cancelled the progress dialog (editor UI only). */
    Cancelled
};

/** One input slot of a Pack request. */
USTRUCT(BlueprintType)
struct TEXTURECHANNELPACKER_API FChannelPackerInput
{
    GENERATED_BODY()

    /** Texture to read. Leave empty to fill the slot with its default (0 for R/G/B, 255 for Alpha). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture Channel Packer")
    TSoftObjectPtr<UTexture2D> Texture;

    /** Which channel of Texture to read. Ignored for single-channel formats (G8/G16/R16F/R32F). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture Channel Packer")
    EChannelPackerChannel SourceChannel = EChannelPackerChannel::Red;

    /** Invert the channel (255 - value). Also applies to the slot default when Texture is empty. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture Channel Packer")
    bool bInvert = false;
};

/** Packs up to four textures into one RGBA texture asset. */
USTRUCT(BlueprintType)
struct TEXTURECHANNELPACKER_API FChannelPackerPackRequest
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture Channel Packer")
    FChannelPackerInput Red;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture Channel Packer")
    FChannelPackerInput Green;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture Channel Packer")
    FChannelPackerInput Blue;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture Channel Packer")
    FChannelPackerInput Alpha;

    /** Output width in pixels (1-16384). Set Width and Height to 0 to use the size of the largest input. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture Channel Packer")
    int32 Width = 0;

    /** Output height in pixels (1-16384). Set Width and Height to 0 to use the size of the largest input. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture Channel Packer")
    int32 Height = 0;

    /** Content folder for the output asset, e.g. "/Game/Textures". */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture Channel Packer")
    FString OutputPath = TEXT("/Game");

    /** Output asset name. Empty = derived from the input names plus FileNameSuffix (the tool's auto naming). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture Channel Packer")
    FString OutputName;

    /** Suffix appended when OutputName is auto-generated (e.g. "_ORM"). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture Channel Packer")
    FString FileNameSuffix = TEXT("_ORM");

    /** Compression option name: "Masks", "Grayscale" or "Default". */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture Channel Packer")
    FString Compression = TEXT("Masks");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture Channel Packer")
    EChannelPackerOverwritePolicy OverwritePolicy = EChannelPackerOverwritePolicy::Fail;

    /** Save the package to disk. The editor tool leaves it dirty instead, for the user to save. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture Channel Packer")
    bool bSave = true;

    /** Validate and resolve the output name and size without reading pixels or writing anything. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture Channel Packer")
    bool bDryRun = false;
};

/** Splits a packed texture into one grayscale texture asset per channel. */
USTRUCT(BlueprintType)
struct TEXTURECHANNELPACKER_API FChannelPackerUnpackRequest
{
    GENERATED_BODY()

    /** The packed texture to split. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture Channel Packer")
    TSoftObjectPtr<UTexture2D> Source;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture Channel Packer")
    bool bExportRed = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture Channel Packer")
    bool bExportGreen = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture Channel Packer")
    bool bExportBlue = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture Channel Packer")
    bool bExportAlpha = true;

    /**
     * Skip channels whose pixels all hold one value (e.g. an unused all-255 alpha), as the
     * Unpack tab does by default. Skipped channels are listed in the result.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture Channel Packer")
    bool bSkipUniformChannels = true;

    /** Content folder for the output assets, e.g. "/Game/Textures". */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture Channel Packer")
    FString OutputPath = TEXT("/Game");

    /** Base name; the per-channel suffix is appended. Empty = derived from the source name (known packed suffix stripped, "T_" enforced). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture Channel Packer")
    FString BaseName;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture Channel Packer")
    FString SuffixRed = TEXT("_R");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture Channel Packer")
    FString SuffixGreen = TEXT("_G");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture Channel Packer")
    FString SuffixBlue = TEXT("_B");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture Channel Packer")
    FString SuffixAlpha = TEXT("_A");

    /** Applies to the request as a whole: with Skip, nothing is written if any output already exists. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture Channel Packer")
    EChannelPackerOverwritePolicy OverwritePolicy = EChannelPackerOverwritePolicy::Fail;

    /** Save the packages to disk. The editor tool leaves them dirty instead, for the user to save. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture Channel Packer")
    bool bSave = true;

    /** Validate, detect uniform channels and resolve output names without writing anything. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture Channel Packer")
    bool bDryRun = false;
};

/** A non-fatal problem reported alongside a result. */
USTRUCT(BlueprintType)
struct TEXTURECHANNELPACKER_API FChannelPackerWarning
{
    GENERATED_BODY()

    /** Stable, locale-independent identifier (e.g. "ErrorUnsupportedFormat"). */
    UPROPERTY(BlueprintReadOnly, Category = "Texture Channel Packer")
    FString Code;

    /** The input slot or channel the warning is about ("R"/"G"/"B"/"A"), or empty. */
    UPROPERTY(BlueprintReadOnly, Category = "Texture Channel Packer")
    FString Channel;

    /** Human-readable description, localized to the editor language. */
    UPROPERTY(BlueprintReadOnly, Category = "Texture Channel Packer")
    FString Message;
};

/** Result of a Pack or Unpack request. */
USTRUCT(BlueprintType)
struct TEXTURECHANNELPACKER_API FChannelPackerResult
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "Texture Channel Packer")
    EChannelPackerStatus Status = EChannelPackerStatus::Failed;

    /** Stable, locale-independent identifier (e.g. "ErrorNoTextures"). Empty unless Status is Failed or Cancelled. */
    UPROPERTY(BlueprintReadOnly, Category = "Texture Channel Packer")
    FString ErrorCode;

    /** Human-readable summary, localized to the editor language. */
    UPROPERTY(BlueprintReadOnly, Category = "Texture Channel Packer")
    FString Message;

    /**
     * Object paths of the output assets (e.g. "/Game/T_Rock_ORM.T_Rock_ORM"): the assets written
     * (and saved, if requested), or, for DryRun and Skipped, the assets the request targets. Empty
     * on failure, except for the channels an unpack request had already written.
     */
    UPROPERTY(BlueprintReadOnly, Category = "Texture Channel Packer")
    TArray<FString> OutputAssets;

    /** Non-fatal problems, e.g. an input in an unsupported format that was filled with the slot default. */
    UPROPERTY(BlueprintReadOnly, Category = "Texture Channel Packer")
    TArray<FChannelPackerWarning> Warnings;

    /** Pack: output size. Unpack: source (and output) size. */
    UPROPERTY(BlueprintReadOnly, Category = "Texture Channel Packer")
    int32 Width = 0;

    UPROPERTY(BlueprintReadOnly, Category = "Texture Channel Packer")
    int32 Height = 0;

    /** Unpack only: requested channels ("R"/"G"/"B"/"A") left out because every pixel holds the same value. */
    UPROPERTY(BlueprintReadOnly, Category = "Texture Channel Packer")
    TArray<FString> SkippedUniformChannels;

    /** True unless the request failed or was cancelled. */
    bool IsSuccess() const
    {
        return Status != EChannelPackerStatus::Failed && Status != EChannelPackerStatus::Cancelled;
    }
};
