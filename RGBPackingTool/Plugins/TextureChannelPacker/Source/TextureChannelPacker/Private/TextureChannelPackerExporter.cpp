#include "TextureChannelPackerExporter.h"
#include "Modules/ModuleManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/FileHelper.h"
#include "Engine/Texture2D.h"
#include "ImageUtils.h"
#include "Math/Float16Color.h"

bool FTextureChannelPackerExporter::ExportTextureToFile(UTexture2D* Texture, const FString& FullPath, bool bIsPNG)
{
	if (!Texture)
	{
		return false;
	}

#if WITH_EDITORONLY_DATA
	int32 Width = Texture->Source.GetSizeX();
	int32 Height = Texture->Source.GetSizeY();
	ETextureSourceFormat Format = Texture->Source.GetFormat();

	// Lock Source Data
	uint8* RawData = Texture->Source.LockMip(0);
	if (!RawData)
	{
		return false;
	}

	TArray<uint8> CompressedData;
	IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));

	if (bIsPNG)
	{
		// For PNG, we need BGRA8 data (sRGB = false/off effectively, but PNG saves as raw color)
		// Since our packing tool creates TSF_BGRA8 by default, we can just save it.
		// However, to be safe, we convert to TArray<FColor> first.

		TArray<FColor> Bitmap;
		Bitmap.SetNumUninitialized(Width * Height);

		// We assume the source is TSF_BGRA8 because our tool creates it that way.
		// If it's not, we might need conversion (similar to GetResizedTextureData logic).
		// For now, let's assume TSF_BGRA8 as per requirements for the output of this tool.
		if (Format == TSF_BGRA8)
		{
			FMemory::Memcpy(Bitmap.GetData(), RawData, Width * Height * sizeof(FColor));
		}
		else
		{
			// Fallback or todo: handle other formats if necessary.
			// Since this function is primarily called after CreateTexture which sets TSF_BGRA8, this should be fine.
			Texture->Source.UnlockMip(0);
			return false;
		}

		TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
		if (ImageWrapper.IsValid() && ImageWrapper->SetRaw(Bitmap.GetData(), Bitmap.Num() * sizeof(FColor), Width, Height, ERGBFormat::BGRA, 8))
		{
			CompressedData = ImageWrapper->GetCompressed();
		}
	}
	else // EXR
	{
		// For EXR, we want Linear 16-bit Float.
		// Our source is BGRA8 (0-255). We need to convert to FFloat16Color.

		TArray<FFloat16Color> FloatBitmap;
		FloatBitmap.SetNumUninitialized(Width * Height);

		if (Format == TSF_BGRA8)
		{
			const FColor* SrcColors = (const FColor*)RawData;
			for (int32 i = 0; i < Width * Height; ++i)
			{
				// Convert 0-255 to 0-1 Linear Float
				// FLinearColor constructor from FColor does sRGB conversion if not careful,
				// but here we want raw value 0-255 mapped to 0-1.
				FLinearColor LinearColor(
					(float)SrcColors[i].R / 255.0f,
					(float)SrcColors[i].G / 255.0f,
					(float)SrcColors[i].B / 255.0f,
					(float)SrcColors[i].A / 255.0f
				);
				FloatBitmap[i] = FFloat16Color(LinearColor);
			}
		}
		else
		{
			Texture->Source.UnlockMip(0);
			return false;
		}

		TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::EXR);
		if (ImageWrapper.IsValid() && ImageWrapper->SetRaw(FloatBitmap.GetData(), FloatBitmap.Num() * sizeof(FFloat16Color), Width, Height, ERGBFormat::RGBAF, 16))
		{
			CompressedData = ImageWrapper->GetCompressed();
		}
	}

	Texture->Source.UnlockMip(0);

	if (CompressedData.Num() > 0)
	{
		return FFileHelper::SaveArrayToFile(CompressedData, *FullPath);
	}

#endif // WITH_EDITORONLY_DATA

	return false;
}
