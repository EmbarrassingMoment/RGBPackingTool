#pragma once

#include "CoreMinimal.h"

class UTexture2D;

class FTextureChannelPackerExporter
{
public:
	/**
	 * Exports the given texture to a file.
	 *
	 * @param Texture The texture asset to export.
	 * @param FullPath The full path to save the file to (including extension).
	 * @param bIsPNG True to export as PNG, false for EXR.
	 * @return True if successful.
	 */
	static bool ExportTextureToFile(UTexture2D* Texture, const FString& FullPath, bool bIsPNG);
};
