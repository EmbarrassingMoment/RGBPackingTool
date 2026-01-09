#include "TextureChannelPackerExporter.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "Misc/FileHelper.h"
#include "Engine/Texture2D.h"
#include "ImageUtils.h"
#include "Math/Float16Color.h"
#include "Logging/LogMacros.h"

DEFINE_LOG_CATEGORY_STATIC(LogTexturePackerExporter, Log, All);

bool FTextureChannelPackerExporter::ExportTextureToFile(UTexture2D* Texture, const FString& FullPath, bool bIsPNG)
{
    if (!Texture)
    {
        UE_LOG(LogTexturePackerExporter, Error, TEXT("ExportTextureToFile: Texture is null."));
        return false;
    }

#if WITH_EDITORONLY_DATA
    int32 SizeX = Texture->Source.GetSizeX();
    int32 SizeY = Texture->Source.GetSizeY();
    // CreateTexture creates TSF_BGRA8, but we should handle robustness just in case.
    ETextureSourceFormat Format = Texture->Source.GetFormat();

    // Lock the source data
    uint8* RawData = Texture->Source.LockMip(0);
    if (!RawData)
    {
        UE_LOG(LogTexturePackerExporter, Error, TEXT("ExportTextureToFile: Failed to lock mip 0."));
        return false;
    }

    TArray<uint8> CompressedData;
    IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));

    if (bIsPNG)
    {
        // PNG export: Expecting 8-bit BGRA
        // Since we know CreateTexture makes BGRA8, we can assume it for now.
        // If the format is G8 (Grayscale), ImageWrapper handles Grayscale.

        ERGBFormat ImageFormat = ERGBFormat::BGRA;
        int32 BitDepth = 8;
        int32 NumChannels = 4;

        if (Format == TSF_G8)
        {
            ImageFormat = ERGBFormat::Gray;
            NumChannels = 1;
        }
        else if (Format != TSF_BGRA8)
        {
            // Fallback for unsupported formats in this simple exporter
            UE_LOG(LogTexturePackerExporter, Warning, TEXT("ExportTextureToFile: Unsupported format for direct PNG export: %d. Assuming BGRA8."), (int32)Format);
        }

        TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);

        if (ImageWrapper.IsValid() && ImageWrapper->SetRaw(RawData, SizeX * SizeY * NumChannels * (BitDepth/8), SizeX, SizeY, ImageFormat, BitDepth))
        {
            CompressedData = ImageWrapper->GetCompressed();
        }
    }
    else // EXR
    {
        // EXR export: 16-bit Float (FFloat16Color), Linear.
        // We need to convert whatever we have to RGBA FFloat16.

        TArray<FFloat16Color> FloatColors;
        FloatColors.SetNumUninitialized(SizeX * SizeY);

        if (Format == TSF_BGRA8)
        {
            const FColor* SourceColors = (const FColor*)RawData; // FColor is BGRA
            for (int32 i = 0; i < SizeX * SizeY; ++i)
            {
                // ReinterpretAsLinear treats the 0-255 values as 0-1 linear floats (R/255.0f, etc.)
                // This preserves the linear data packed in CreateTexture.
                FloatColors[i] = FFloat16Color(SourceColors[i].ReinterpretAsLinear());
            }
        }
        else if (Format == TSF_G8)
        {
             const uint8* GrayData = RawData;
             for (int32 i = 0; i < SizeX * SizeY; ++i)
             {
                 float Val = (float)GrayData[i] / 255.0f;
                 FloatColors[i] = FFloat16Color(FLinearColor(Val, Val, Val, 1.0f));
             }
        }
        else
        {
            UE_LOG(LogTexturePackerExporter, Error, TEXT("ExportTextureToFile: Unsupported format for EXR export: %d"), (int32)Format);
            Texture->Source.UnlockMip(0);
            return false;
        }

        TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::EXR);
        if (ImageWrapper.IsValid() && ImageWrapper->SetRaw(FloatColors.GetData(), FloatColors.Num() * sizeof(FFloat16Color), SizeX, SizeY, ERGBFormat::RGBAF, 16))
        {
            CompressedData = ImageWrapper->GetCompressed();
        }
    }

    Texture->Source.UnlockMip(0);

    if (CompressedData.Num() > 0)
    {
        return FFileHelper::SaveArrayToFile(CompressedData, *FullPath);
    }
#else
    UE_LOG(LogTexturePackerExporter, Error, TEXT("ExportTextureToFile: Editor Only."));
#endif

    return false;
}
