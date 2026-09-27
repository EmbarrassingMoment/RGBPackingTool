#pragma once

#include "CoreMinimal.h"
#include "Engine/Texture.h"
#include "Math/Float16.h"
#include "TextureChannelPacker.h"

class UTexture2D;

DECLARE_LOG_CATEGORY_EXTERN(LogTexturePacker, Log, All);

/**
 * Shared utilities used by both the Pack tab (FTextureChannelPackerModule) and the
 * Unpack tab (FTextureChannelUnpacker). Kept in a namespace to avoid symbol clashes
 * with other modules in monolithic builds.
 */
namespace TextureChannelPackerUtils
{
    /** Resolution above which a memory-consumption warning dialog is shown before processing. */
    inline constexpr int32 LargeTextureWarningThreshold = 8192;

    /** Maximum supported texture dimension (DirectX 11 / OpenGL 4.1+ limit). */
    inline constexpr int32 MaxTextureDimension = 16384;

    /**
     * @brief Logs the wall-clock duration of a processing phase when it goes out of scope.
     *
     * Emits "[Perf] <Label>: <ms> ms" on LogTexturePacker so phase costs can be read from
     * the Output Log during normal use and collected by the perf automation test.
     * The overhead is two timestamp reads per phase, so it stays enabled in all builds.
     */
    class FPhaseTimer
    {
    public:
        explicit FPhaseTimer(const TCHAR* InLabel)
            : Label(InLabel)
            , StartSeconds(FPlatformTime::Seconds())
        {
        }

        ~FPhaseTimer()
        {
            const double Milliseconds = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
            UE_LOG(LogTexturePacker, Log, TEXT("[Perf] %s: %.1f ms"), Label, Milliseconds);
        }

        FPhaseTimer(const FPhaseTimer&) = delete;
        FPhaseTimer& operator=(const FPhaseTimer&) = delete;

    private:
        const TCHAR* Label;
        double StartSeconds;
    };

    /**
     * @brief Retrieves a localized message based on the current culture.
     *
     * Returns either the Japanese text (if the current culture is Japanese)
     * or the English text (for all other cultures).
     *
     * @param Key A unique identifier for the localization key (currently unused but good for future expansion).
     * @param EnglishText The text to display in English.
     * @param JapaneseText The text to display in Japanese.
     * @return FText The localized text.
     */
    FText GetLocalizedMessage(const FString& Key, const FString& EnglishText, const FString& JapaneseText);

    /**
     * @brief Displays a notification toast in the editor.
     *
     * @param Message The text message to display.
     * @param bSuccess If true, shows a success icon; otherwise, shows an error icon.
     */
    void ShowNotification(const FText& Message, bool bSuccess);

    /** Short single-letter label for a source channel (R/G/B/A) for compact UI. */
    FString SourceChannelToShortString(ESourceChannel Ch);

    /**
     * @brief Returns the byte offset of the requested channel within a BGRA8 pixel.
     *
     * BGRA8 lays out bytes as [B, G, R, A], so Red=2, Green=1, Blue=0, Alpha=3.
     */
    int32 GetBGRAChannelOffset(ESourceChannel Channel);

    /** Extracts the requested channel value from an FColor. */
    uint8 ExtractChannelFromFColor(const FColor& C, ESourceChannel Channel);

    /**
     * @brief Returns true for source formats that physically only carry one channel of data.
     *
     * For these formats, the channel selector is meaningless — the lone luminance value is
     * always used regardless of which output channel the user picks.
     */
    bool IsSingleChannelFormat(ETextureSourceFormat Format);

    /**
     * @struct FLockedTextureSource
     * @brief Read-only view of a UTexture2D's mip-0 source data, held locked until released.
     *
     * Replaces the former full-resolution copy of the source bytes: workers read straight
     * from the locked mip, so locking costs nothing regardless of texture size. The lock is
     * taken with LockMipReadOnly, which (unlike LockMip) does not re-hash the payload or
     * regenerate the source GUID of the input asset on unlock.
     *
     * Lock() and Release() (or destruction) must happen on the Game Thread; reading Data from
     * worker threads in between is allowed by the engine's texture threading rules. Read locks
     * are recursive, so the same texture assigned to several slots can be locked several times.
     * The caller must keep the texture alive while the lock is held.
     */
    struct FLockedTextureSource
    {
        const uint8* Data = nullptr;
        int32 Width = 0;
        int32 Height = 0;
        ETextureSourceFormat Format = TSF_Invalid;
        FString TextureName;
        bool bIsValid = false;

        /**
         * User-facing error message if locking failed.
         * Empty if no error occurred.
         */
        FText ErrorMessage;

        FLockedTextureSource() = default;
        ~FLockedTextureSource() { Release(); }

        FLockedTextureSource(FLockedTextureSource&& Other) { *this = MoveTemp(Other); }
        FLockedTextureSource& operator=(FLockedTextureSource&& Other);

        FLockedTextureSource(const FLockedTextureSource&) = delete;
        FLockedTextureSource& operator=(const FLockedTextureSource&) = delete;

        /**
         * @brief Locks mip 0 of SourceTex read-only and validates its dimensions and format.
         *
         * A null texture yields an invalid (but error-free) result, which the processing
         * step turns into a default-filled channel. Must be called on the Game Thread.
         */
        static FLockedTextureSource Lock(UTexture2D* SourceTex);

        /** Unlocks the source mip. Must be called on the Game Thread; safe to call repeatedly. */
        void Release();

    private:
        UTexture2D* LockedTexture = nullptr;
    };

    /**
     * @brief Builds a per-pixel channel sampler for the given source format and hands it to Visitor.
     *
     * The sampler reads a single channel value straight out of the locked mip and converts it to
     * 8-bit, so neither the pack nor the unpack path needs an intermediate full-resolution copy
     * or FColor buffer. Dispatching on the format once (outside the pixel loop) keeps the inner
     * loop free of per-pixel branching on the format.
     *
     * Sampler signature: uint8 (int64 PixelIndex, int32 ChannelIndex), where ChannelIndex is
     * 0=R, 1=G, 2=B, 3=A. Pixel indices are 64-bit, so sources larger than 2 GB are handled.
     *
     * Single-channel formats (G8/G16/R16F/R32F) replicate their lone value across R/G/B and
     * report an opaque (255) Alpha. The pack path remaps its channel selection to R for these
     * formats beforehand (see ExtractChannelToG8), so a grayscale mask assigned to the Alpha
     * slot still packs its luminance.
     *
     * @return False if the source format is unsupported (Visitor is then not called).
     */
    template <typename FVisitor>
    bool VisitChannelSampler(const uint8* Src, ETextureSourceFormat Format, FVisitor&& Visitor)
    {
        // Byte offsets of R, G, B, A within a BGRA8 pixel (see GetBGRAChannelOffset).
        static const int32 BGRAOffsets[4] = { 2, 1, 0, 3 };

        switch (Format)
        {
        case TSF_BGRA8:
        {
            Visitor([Src](int64 PixelIndex, int32 Channel) -> uint8
            {
                return Src[PixelIndex * 4 + BGRAOffsets[Channel]];
            });
            return true;
        }
        case TSF_G8:
        {
            Visitor([Src](int64 PixelIndex, int32 Channel) -> uint8
            {
                return Channel == 3 ? (uint8)255 : Src[PixelIndex];
            });
            return true;
        }
        case TSF_G16:
        {
            const uint16* Pixels = (const uint16*)Src;
            Visitor([Pixels](int64 PixelIndex, int32 Channel) -> uint8
            {
                return Channel == 3 ? (uint8)255 : (uint8)(Pixels[PixelIndex] >> 8);
            });
            return true;
        }
        case TSF_R16F:
        {
            const FFloat16* Pixels = (const FFloat16*)Src;
            Visitor([Pixels](int64 PixelIndex, int32 Channel) -> uint8
            {
                if (Channel == 3)
                {
                    return 255;
                }
                return (uint8)FMath::Clamp<float>((float)Pixels[PixelIndex] * 255.0f, 0.0f, 255.0f);
            });
            return true;
        }
        case TSF_R32F:
        {
            const float* Pixels = (const float*)Src;
            Visitor([Pixels](int64 PixelIndex, int32 Channel) -> uint8
            {
                if (Channel == 3)
                {
                    return 255;
                }
                return (uint8)FMath::Clamp<float>(Pixels[PixelIndex] * 255.0f, 0.0f, 255.0f);
            });
            return true;
        }
        case TSF_RGBA32F:
        {
            const FLinearColor* Pixels = (const FLinearColor*)Src;
            Visitor([Pixels](int64 PixelIndex, int32 Channel) -> uint8
            {
                const FLinearColor& LC = Pixels[PixelIndex];
                float Value;
                switch (Channel)
                {
                case 0:  Value = LC.R; break;
                case 1:  Value = LC.G; break;
                case 2:  Value = LC.B; break;
                default: Value = LC.A; break;
                }
                return (uint8)FMath::Clamp<float>(Value * 255.0f, 0.0f, 255.0f);
            });
            return true;
        }
        default:
            return false;
        }
    }

    /**
     * @brief Copies one channel of a locked source into an 8-bit buffer at source resolution.
     *
     * Rows are processed in parallel with a plain inner loop, so the only allocation is the
     * one-byte-per-pixel output. For single-channel formats the channel selection is ignored
     * and the lone value is used, whichever slot it is packed into.
     *
     * @return False if the input is invalid or its format is unsupported.
     */
    bool ExtractChannelToG8(const FLockedTextureSource& Input, ESourceChannel SourceChannel, TArray<uint8>& OutChannel);

    /**
     * @struct FTextureProcessResult
     * @brief Represents the result of a texture processing operation.
     *
     * This struct contains the processed pixel data for a specific channel or an error message
     * if the operation failed. It is generated by background threads and consumed by the Game Thread.
     */
    struct FTextureProcessResult
    {
        TArray<uint8> ProcessedData;
        FText ErrorMessage;
        bool bSuccess = true;
    };

    /**
     * @brief Produces one 8-bit channel at the target resolution from a locked source.
     *
     * Extracts the selected channel straight from the locked mip (ExtractChannelToG8) and,
     * if the target size differs, resizes that single channel with FImageCore::ResizeImage
     * (Box filter, multi-threaded). Resizing after extraction is exact because the filter is
     * linear and per-channel, and it costs a quarter of resizing all four channels as FColor.
     * This function is thread-safe and is run in parallel across the four slots.
     *
     * @param Input The locked source of the input texture (may be invalid for an empty slot).
     * @param TargetWidth The target width for the output.
     * @param TargetHeight The target height for the output.
     * @param SourceChannel Which channel of the input to read (R/G/B/A). Ignored for single-channel formats.
     * @return FTextureProcessResult The processed single-channel 8-bit data. An invalid or unsupported input
     *         yields empty ProcessedData so the caller can substitute the slot default (0 for R/G/B, 255 for Alpha).
     */
    FTextureProcessResult ProcessTextureSourceData(const FLockedTextureSource& Input, int32 TargetWidth, int32 TargetHeight, ESourceChannel SourceChannel);
}
