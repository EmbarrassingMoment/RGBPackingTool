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
#include "ImageCore.h"
#include "Math/UnrealMathUtility.h"
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

FLockedTextureSource& FLockedTextureSource::operator=(FLockedTextureSource&& Other)
{
    if (this != &Other)
    {
        Release();

        Data = Other.Data;
        Width = Other.Width;
        Height = Other.Height;
        Format = Other.Format;
        TextureName = MoveTemp(Other.TextureName);
        bIsValid = Other.bIsValid;
        ErrorMessage = MoveTemp(Other.ErrorMessage);
        ErrorCode = MoveTemp(Other.ErrorCode);
        LockedTexture = Other.LockedTexture;

        Other.Data = nullptr;
        Other.bIsValid = false;
        Other.LockedTexture = nullptr;
    }
    return *this;
}

void FLockedTextureSource::Release()
{
#if WITH_EDITORONLY_DATA
    if (LockedTexture)
    {
        LockedTexture->Source.UnlockMip(0);
    }
#endif
    LockedTexture = nullptr;
    Data = nullptr;
    bIsValid = false;
}

FLockedTextureSource FLockedTextureSource::Lock(UTexture2D* SourceTex)
{
    FLockedTextureSource Result;
    if (!SourceTex)
    {
        return Result;
    }

    Result.TextureName = SourceTex->GetName();

#if WITH_EDITORONLY_DATA
    Result.Width = SourceTex->Source.GetSizeX();
    Result.Height = SourceTex->Source.GetSizeY();
    Result.Format = SourceTex->Source.GetFormat();

    const int32 BytesPerPixel = SourceTex->Source.GetBytesPerPixel();
    if (BytesPerPixel == 0)
    {
        UE_LOG(LogTexturePacker, Error,
            TEXT("GetBytesPerPixel() returned 0 for texture: %s (Format: %d). This format may not be supported."),
            *Result.TextureName, (int32)Result.Format);
        return Result;  // Return invalid result
    }

    // Compute the pixel count in 64-bit to avoid int32 overflow. Every per-channel buffer
    // is one byte per pixel and TArray is int32-indexed, so the pixel count itself must fit.
    const int64 NumPixels = (int64)Result.Width * (int64)Result.Height;

    if (NumPixels <= 0)
    {
        UE_LOG(LogTexturePacker, Error,
            TEXT("Invalid source dimensions for texture: %s (Width: %d, Height: %d, BPP: %d)"),
            *Result.TextureName, Result.Width, Result.Height, BytesPerPixel);
        return Result;  // Return invalid result
    }

    if (NumPixels > (int64)MAX_int32)
    {
        UE_LOG(LogTexturePacker, Error,
            TEXT("Texture too large to process: %s (Width: %d, Height: %d, Pixels: %lld)"),
            *Result.TextureName, Result.Width, Result.Height, NumPixels);
        Result.ErrorCode = TEXT("ErrorTextureTooLarge");
        Result.ErrorMessage = GetLocalizedMessage(
            TEXT("ErrorTextureTooLarge"),
            TEXT("Input texture is too large to process. Reduce its resolution."),
            TEXT("入力テクスチャが大きすぎて処理できません。解像度を下げてください。")
        );
        return Result;  // Return invalid result
    }

    // Read-only lock: the data is only ever read, and a write lock would re-hash the payload
    // and regenerate the input asset's source GUID on unlock.
    const uint8* SrcData = SourceTex->Source.LockMipReadOnly(0);
    if (!SrcData)
    {
        UE_LOG(LogTexturePacker, Warning, TEXT("Failed to lock source mip for texture: %s"), *Result.TextureName);
        Result.ErrorCode = TEXT("ErrorLockFailed");
        Result.ErrorMessage = GetLocalizedMessage(
            TEXT("ErrorLockFailed"),
            TEXT("Failed to access texture data. The texture may be corrupted or in use. Try reimporting the texture."),
            TEXT("テクスチャデータへのアクセスに失敗しました。テクスチャが破損しているか、使用中の可能性があります。テクスチャを再インポートしてください。")
        );
        return Result;
    }

    Result.Data = SrcData;
    Result.LockedTexture = SourceTex;
    Result.bIsValid = true;
#else
    UE_LOG(LogTexturePacker, Error, TEXT("TextureChannelPacker requires WITH_EDITORONLY_DATA to access Source."));
    Result.ErrorCode = TEXT("ErrorNoEditorData");
    Result.ErrorMessage = GetLocalizedMessage(
        TEXT("ErrorNoEditorData"),
        TEXT("This plugin requires Editor-only data to function. Ensure the project is built with editor support."),
        TEXT("このプラグインはエディター専用データが必要です。プロジェクトがエディターサポート付きでビルドされていることを確認してください。")
    );
#endif

    return Result;
}

bool ExtractChannelToG8(const FLockedTextureSource& Input, ESourceChannel SourceChannel, TArray<uint8>& OutChannel)
{
    if (!Input.bIsValid || !Input.Data)
    {
        return false;
    }

    const int32 Width = Input.Width;
    const int32 Height = Input.Height;

    // Single-channel formats carry one value; use it for whichever slot the user picked
    // (including Alpha) instead of the sampler's opaque-alpha convention.
    const int32 ChannelIndex = IsSingleChannelFormat(Input.Format) ? 0 : (int32)SourceChannel;

    OutChannel.SetNumUninitialized(Width * Height);
    uint8* Dest = OutChannel.GetData();

    return VisitChannelSampler(Input.Data, Input.Format, [&](auto&& Sample)
    {
        // One task per row with a plain inner loop, so the sampler is inlined and the
        // compiler can vectorize the strided reads.
        ParallelFor(Height, [&](int32 Y)
        {
            const int64 RowStart = (int64)Y * Width;
            uint8* DestRow = Dest + RowStart;
            for (int32 X = 0; X < Width; ++X)
            {
                DestRow[X] = Sample(RowStart + X, ChannelIndex);
            }
        });
    });
}

FTextureProcessResult ProcessTextureSourceData(const FLockedTextureSource& Input, int32 TargetWidth, int32 TargetHeight, ESourceChannel SourceChannel)
{
    FTextureProcessResult Result;
    const int32 NumTargetPixels = TargetWidth * TargetHeight;

    if (!Input.bIsValid)
    {
        // Empty/invalid input: return no data so the caller substitutes the slot default
        // (black for R/G/B, opaque white for Alpha) and reports any error message.
        return Result;
    }

    // Pull the selected channel out of the locked mip at source resolution (1 byte per pixel).
    TArray<uint8> SourceChannel8;
    if (!ExtractChannelToG8(Input, SourceChannel, SourceChannel8))
    {
        UE_LOG(LogTexturePacker, Error, TEXT("Unsupported Source Format: %d for texture: %s"), (int32)Input.Format, *Input.TextureName);
        Result.bSuccess = false;
        Result.ErrorCode = TEXT("ErrorUnsupportedFormat");
        Result.ErrorMessage = GetLocalizedMessage(
            TEXT("ErrorUnsupportedFormat"),
            TEXT("Texture format not supported. Please convert to PNG or TGA."),
            TEXT("テクスチャ形式がサポートされていません。PNGまたはTGAに変換してください。")
        );
        return Result;
    }

    if (Input.Width == TargetWidth && Input.Height == TargetHeight)
    {
        Result.ProcessedData = MoveTemp(SourceChannel8);
        return Result;
    }

    // Resize the single 8-bit channel. The data is linear (mask/data, not color), so the
    // filter runs on the raw values just like the previous box average did.
    Result.ProcessedData.SetNumUninitialized(NumTargetPixels);
    const FImageView SourceView((void*)SourceChannel8.GetData(), Input.Width, Input.Height, 1, ERawImageFormat::G8, EGammaSpace::Linear);
    const FImageView TargetView(Result.ProcessedData.GetData(), TargetWidth, TargetHeight, 1, ERawImageFormat::G8, EGammaSpace::Linear);
    FImageCore::ResizeImage(SourceView, TargetView, FImageCore::EResizeImageFilter::Box);

    return Result;
}

} // namespace TextureChannelPackerUtils
