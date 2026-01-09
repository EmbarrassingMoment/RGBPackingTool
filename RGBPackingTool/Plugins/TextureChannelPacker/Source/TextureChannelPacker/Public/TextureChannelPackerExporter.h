#pragma once

#include "CoreMinimal.h"

class UTexture2D;

class FTextureChannelPackerExporter
{
public:
    /**
     * Exports a UTexture2D to a file (PNG or EXR).
     * @param Texture The texture asset to export.
     * @param FullPath The full file path to save to (including extension).
     * @param bIsPNG True for PNG (8-bit), False for EXR (16-bit Float).
     * @return True if successful.
     */
    static bool ExportTextureToFile(UTexture2D* Texture, const FString& FullPath, bool bIsPNG);
};
