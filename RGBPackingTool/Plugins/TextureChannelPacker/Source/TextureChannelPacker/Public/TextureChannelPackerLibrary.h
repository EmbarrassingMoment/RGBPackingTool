#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "TextureChannelPackerTypes.h"
#include "TextureChannelPackerLibrary.generated.h"

/**
 * @class UTextureChannelPackerLibrary
 * @brief Pack / Unpack without the editor UI, for Blueprint, Editor Python and Remote Control.
 *
 * Runs the same pipeline as the Texture Channel Packer tool, but never opens a dialog or shows a
 * notification: everything is reported in the returned FChannelPackerResult. Editor-only; call it
 * on the Game Thread (Editor Utility Blueprints, Editor Python, Remote Control calls all are).
 *
 * Python:
 *   red = unreal.ChannelPackerInput()
 *   red.texture = unreal.load_asset("/Game/Rock/T_Rock_AO")
 *   request = unreal.ChannelPackerPackRequest()
 *   request.red = red
 *   request.output_path = "/Game/Rock"
 *   result = unreal.TextureChannelPackerLibrary.pack_textures(request)
 */
UCLASS()
class TEXTURECHANNELPACKER_API UTextureChannelPackerLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    /** Packs up to four textures into one RGBA texture asset. See FChannelPackerPackRequest. */
    UFUNCTION(BlueprintCallable, Category = "Texture Channel Packer")
    static FChannelPackerResult PackTextures(const FChannelPackerPackRequest& Request);

    /** Splits a texture into one grayscale texture asset per channel. See FChannelPackerUnpackRequest. */
    UFUNCTION(BlueprintCallable, Category = "Texture Channel Packer")
    static FChannelPackerResult UnpackTexture(const FChannelPackerUnpackRequest& Request);

    /**
     * Returns a pack request pre-filled with a preset's defaults (source channels, invert flags,
     * compression, filename suffix). Returns false and leaves OutRequest at its defaults if no
     * preset (built-in or user) has that name.
     */
    UFUNCTION(BlueprintCallable, Category = "Texture Channel Packer")
    static bool MakePackRequestFromPreset(const FString& PresetName, FChannelPackerPackRequest& OutRequest);

    /**
     * Returns an unpack request pre-filled with a preset's per-channel suffixes. Returns false and
     * leaves OutRequest at its defaults if no preset (built-in or user) has that name.
     */
    UFUNCTION(BlueprintCallable, Category = "Texture Channel Packer")
    static bool MakeUnpackRequestFromPreset(const FString& PresetName, FChannelPackerUnpackRequest& OutRequest);

    /** Names of the built-in and user presets. */
    UFUNCTION(BlueprintCallable, Category = "Texture Channel Packer")
    static TArray<FString> GetPresetNames();
};
