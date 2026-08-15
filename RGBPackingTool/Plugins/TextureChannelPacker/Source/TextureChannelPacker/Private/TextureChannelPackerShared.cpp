#include "TextureChannelPackerShared.h"
#include "Engine/Texture2D.h"
// FTexturePlatformData lives in a dedicated header on newer engine versions but is
// declared inside Engine/Texture.h (pulled in via Engine/Texture2D.h above) on older
// ones. Guard the include so the build succeeds regardless of where it resides.
#if __has_include("Engine/TexturePlatformData.h")
#include "Engine/TexturePlatformData.h"
#endif
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Styling/AppStyle.h"
#include "ImageUtils.h"
#include "Math/UnrealMathUtility.h"
#include "Math/Float16.h"
#include "Async/ParallelFor.h"
#include "Internationalization/Internationalization.h"
#include "Internationalization/Culture.h"

DEFINE_LOG_CATEGORY(LogTexturePacker);

namespace TextureChannelPackerUtils
{

FText GetLocalizedMessage(const FString& Key, const FString& EnglishText, const FString& JapaneseText)
{
    FString CultureName = FInternationalization::Get().GetCurrentCulture()->GetTwoLetterISOLanguageName();
    if (CultureName == TEXT("ja"))
    {
        return FText::FromString(JapaneseText);
    }
    // We return FText::FromString to avoid unsafe usage of internal localization macros with dynamic strings.
    return FText::FromString(EnglishText);
}

void ShowNotification(const FText& Message, bool bSuccess)
{
    FNotificationInfo Info(Message);
    Info.ExpireDuration = 3.0f;

    if (bSuccess)
    {
        Info.Image = FAppStyle::GetBrush("Icons.SuccessWithColor");
    }
    else
    {
        Info.Image = FAppStyle::GetBrush("Icons.ErrorWithColor");
    }

    TSharedPtr<SNotificationItem> NotificationItem = FSlateNotificationManager::Get().AddNotification(Info);
    if (NotificationItem.IsValid())
    {
        NotificationItem->SetCompletionState(bSuccess ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
        NotificationItem->ExpireAndFadeout();
    }
}

FString SourceChannelToShortString(ESourceChannel Ch)
{
    switch (Ch)
    {
    case ESourceChannel::Red:   return TEXT("R");
    case ESourceChannel::Green: return TEXT("G");
    case ESourceChannel::Blue:  return TEXT("B");
    case ESourceChannel::Alpha: return TEXT("A");
    }
    return TEXT("R");
}

int32 GetBGRAChannelOffset(ESourceChannel Channel)
{
    switch (Channel)
    {
    case ESourceChannel::Red:   return 2;
    case ESourceChannel::Green: return 1;
    case ESourceChannel::Blue:  return 0;
    case ESourceChannel::Alpha: return 3;
    default:                    return 2;
    }
}

uint8 ExtractChannelFromFColor(const FColor& C, ESourceChannel Channel)
{
    switch (Channel)
    {
    case ESourceChannel::Red:   return C.R;
    case ESourceChannel::Green: return C.G;
    case ESourceChannel::Blue:  return C.B;
    case ESourceChannel::Alpha: return C.A;
    default:                    return C.R;
    }
}

bool IsSingleChannelFormat(ETextureSourceFormat Format)
{
    return Format == TSF_G8 || Format == TSF_G16 || Format == TSF_R16F || Format == TSF_R32F;
}

FTextureRawData ExtractTextureSourceData(UTexture2D* SourceTex)
{
    FTextureRawData Result;
    if (!SourceTex)
    {
        return Result;
    }

    Result.TextureName = SourceTex->GetName();

#if WITH_EDITORONLY_DATA
    Result.Width = SourceTex->Source.GetSizeX();
    Result.Height = SourceTex->Source.GetSizeY();
    Result.Format = SourceTex->Source.GetFormat();

    uint8* SrcData = SourceTex->Source.LockMip(0);
    if (SrcData)
    {
        int32 BytesPerPixel = SourceTex->Source.GetBytesPerPixel();

        // Validation 1: Check if BytesPerPixel is valid
        if (BytesPerPixel == 0)
        {
            UE_LOG(LogTexturePacker, Error,
                TEXT("GetBytesPerPixel() returned 0 for texture: %s (Format: %d). This format may not be supported."),
                *Result.TextureName, (int32)Result.Format);
            SourceTex->Source.UnlockMip(0);
            return Result;  // Return invalid result
        }

        // Compute the byte count in 64-bit to avoid int32 overflow. A 16K RGBA32F texture
        // is 16384*16384*16 = 4 GB, which silently wraps to a small/negative value in int32
        // (e.g. exactly 0), producing a confusing "invalid total bytes" failure. TArray is
        // int32-indexed, so anything larger than INT32_MAX cannot be held regardless.
        const int64 TotalBytes = (int64)Result.Width * (int64)Result.Height * (int64)BytesPerPixel;

        // Validation 2: Check if TotalBytes is valid
        if (TotalBytes <= 0)
        {
            UE_LOG(LogTexturePacker, Error,
                TEXT("Invalid total bytes (%lld) for texture: %s (Width: %d, Height: %d, BPP: %d)"),
                TotalBytes, *Result.TextureName, Result.Width, Result.Height, BytesPerPixel);
            SourceTex->Source.UnlockMip(0);
            return Result;  // Return invalid result
        }

        // Validation 3: Reject textures too large for a 32-bit-indexed TArray.
        if (TotalBytes > (int64)MAX_int32)
        {
            UE_LOG(LogTexturePacker, Error,
                TEXT("Texture too large to process: %s (Width: %d, Height: %d, BPP: %d, Bytes: %lld)"),
                *Result.TextureName, Result.Width, Result.Height, BytesPerPixel, TotalBytes);
            SourceTex->Source.UnlockMip(0);
            Result.ErrorMessage = GetLocalizedMessage(
                TEXT("ErrorTextureTooLarge"),
                TEXT("Input texture is too large to process. Reduce its resolution or use a format with fewer bytes per pixel (e.g. 8-bit instead of 32-bit float)."),
                TEXT("入力テクスチャが大きすぎて処理できません。解像度を下げるか、ピクセルあたりのバイト数が少ない形式（32bit float ではなく 8bit など）を使用してください。")
            );
            return Result;  // Return invalid result
        }

        // Data is valid, proceed with copy
        Result.RawData.SetNumUninitialized((int32)TotalBytes);
        FMemory::Memcpy(Result.RawData.GetData(), SrcData, TotalBytes);
        Result.bIsValid = true;
    }
    else
    {
        UE_LOG(LogTexturePacker, Warning, TEXT("Failed to lock source mip for texture: %s"), *Result.TextureName);
        Result.ErrorMessage = GetLocalizedMessage(
            TEXT("ErrorLockFailed"),
            TEXT("Failed to access texture data. The texture may be corrupted or in use. Try reimporting the texture."),
            TEXT("テクスチャデータへのアクセスに失敗しました。テクスチャが破損しているか、使用中の可能性があります。テクスチャを再インポートしてください。")
        );
    }
    SourceTex->Source.UnlockMip(0);
#else
    UE_LOG(LogTexturePacker, Error, TEXT("TextureChannelPacker requires WITH_EDITORONLY_DATA to access Source."));
    Result.ErrorMessage = GetLocalizedMessage(
        TEXT("ErrorNoEditorData"),
        TEXT("This plugin requires Editor-only data to function. Ensure the project is built with editor support."),
        TEXT("このプラグインはエディター専用データが必要です。プロジェクトがエディターサポート付きでビルドされていることを確認してください。")
    );
#endif

    return Result;
}

FTextureProcessResult ProcessTextureSourceData(FTextureRawData& Input, int32 TargetWidth, int32 TargetHeight, ESourceChannel SourceChannel)
{
    FTextureProcessResult Result;
    // Default to zero-filled array
    Result.ProcessedData.Init(0, TargetWidth * TargetHeight);

    if (!Input.bIsValid)
    {
        return Result; // Empty/Invalid input results in black channel (or white if handled by caller default)
    }

    int32 SrcWidth = Input.Width;
    int32 SrcHeight = Input.Height;
    int32 NumPixels = SrcWidth * SrcHeight;
    const uint8* SrcData = Input.RawData.GetData();

    // Optimization: Fast path for same-resolution textures
    if (SrcWidth == TargetWidth && SrcHeight == TargetHeight)
    {
        if (Input.Format == TSF_G8)
        {
            // Direct move for Grayscale input (zero-copy optimization).
            // Channel selection is moot for single-channel data.
            Result.ProcessedData = MoveTemp(Input.RawData);
            return Result;
        }
        else if (Input.Format == TSF_BGRA8)
        {
            // Parallel channel extraction for BGRA input
            Result.ProcessedData.SetNumUninitialized(NumPixels);
            uint8* DestData = Result.ProcessedData.GetData();
            const uint8* SrcPtr = SrcData;
            const int32 ChannelOffset = GetBGRAChannelOffset(SourceChannel);

            ParallelFor(NumPixels, [DestData, SrcPtr, ChannelOffset](int32 i)
            {
                DestData[i] = SrcPtr[i * 4 + ChannelOffset];
            });
            return Result;
        }
    }

    TArray<FColor> SrcColors;
    SrcColors.SetNumUninitialized(NumPixels);

    // Convert input to FColor (RGBA values stored in FColor's R/G/B/A members).
    // For single-channel formats we replicate the value across R/G/B so that channel
    // selection still produces the expected result.
    switch (Input.Format)
    {
    case TSF_BGRA8:
    {
        FMemory::Memcpy(SrcColors.GetData(), SrcData, Input.RawData.Num());
        break;
    }
    case TSF_G8:
    {
        const uint8* GrayData = SrcData;
        for (int32 i = 0; i < NumPixels; ++i)
        {
            uint8 Val = GrayData[i];
            SrcColors[i] = FColor(Val, Val, Val, 255);
        }
        break;
    }
    case TSF_G16:
    {
        // 16-bit Grayscale: 2 bytes per pixel
        const uint16* GrayData16 = (const uint16*)SrcData;
        for (int32 i = 0; i < NumPixels; ++i)
        {
            uint8 Val = (uint8)(GrayData16[i] >> 8);
            SrcColors[i] = FColor(Val, Val, Val, 255);
        }
        break;
    }
    case TSF_R16F:
    {
        // Half-float: 2 bytes per pixel
        const FFloat16* Pixel16 = (const FFloat16*)SrcData;
        for (int32 i = 0; i < NumPixels; ++i)
        {
            uint8 Val = (uint8)FMath::Clamp<float>((float)Pixel16[i] * 255.0f, 0.0f, 255.0f);
            SrcColors[i] = FColor(Val, Val, Val, 255);
        }
        break;
    }
    case TSF_R32F:
    {
        // Float: 4 bytes per pixel
        const float* Pixel32 = (const float*)SrcData;
        for (int32 i = 0; i < NumPixels; ++i)
        {
            uint8 Val = (uint8)FMath::Clamp<float>(Pixel32[i] * 255.0f, 0.0f, 255.0f);
            SrcColors[i] = FColor(Val, Val, Val, 255);
        }
        break;
    }
    case TSF_RGBA32F:
    {
        // Linear Color: 16 bytes per pixel. Preserve all four channels so the user can pick any.
        const FLinearColor* LinearColors = (const FLinearColor*)SrcData;
        for (int32 i = 0; i < NumPixels; ++i)
        {
            const FLinearColor& LC = LinearColors[i];
            uint8 R = (uint8)FMath::Clamp<float>(LC.R * 255.0f, 0.0f, 255.0f);
            uint8 G = (uint8)FMath::Clamp<float>(LC.G * 255.0f, 0.0f, 255.0f);
            uint8 B = (uint8)FMath::Clamp<float>(LC.B * 255.0f, 0.0f, 255.0f);
            uint8 A = (uint8)FMath::Clamp<float>(LC.A * 255.0f, 0.0f, 255.0f);
            SrcColors[i] = FColor(R, G, B, A);
        }
        break;
    }
    default:
    {
        UE_LOG(LogTexturePacker, Error, TEXT("Unsupported Source Format: %d for texture: %s"), (int32)Input.Format, *Input.TextureName);
        Result.bSuccess = false;
        Result.ErrorMessage = GetLocalizedMessage(
            TEXT("ErrorUnsupportedFormat"),
            TEXT("Texture format not supported. Please convert to PNG or TGA."),
            TEXT("テクスチャ形式がサポートされていません。PNGまたはTGAに変換してください。")
        );
        return Result;
    }
    }

    // Resize if necessary
    TArray<FColor> ResizedColors;
    if (SrcWidth != TargetWidth || SrcHeight != TargetHeight)
    {
        ResizedColors.SetNum(TargetWidth * TargetHeight);
        FImageUtils::ImageResize(SrcWidth, SrcHeight, SrcColors, TargetWidth, TargetHeight, ResizedColors, false);
    }
    else
    {
        ResizedColors = MoveTemp(SrcColors);
    }

    // For single-channel source formats, the channel selection has no effect (R=G=B).
    // We always read R to keep the inner loop branch-free.
    const ESourceChannel EffectiveChannel = IsSingleChannelFormat(Input.Format) ? ESourceChannel::Red : SourceChannel;

    // Convert FColor to uint8 array (single channel, 1 byte per pixel)
    Result.ProcessedData.SetNumUninitialized(TargetWidth * TargetHeight);
    uint8* DestData = Result.ProcessedData.GetData();
    for (int32 i = 0; i < TargetWidth * TargetHeight; ++i)
    {
        DestData[i] = ExtractChannelFromFColor(ResizedColors[i], EffectiveChannel);
    }

    return Result;
}

} // namespace TextureChannelPackerUtils
