#include "TextureChannelPackerLibrary.h"
#include "TextureChannelPackerCore.h"

FChannelPackerResult UTextureChannelPackerLibrary::PackTextures(const FChannelPackerPackRequest& Request)
{
    return TextureChannelPackerCore::Pack(Request);
}

FChannelPackerResult UTextureChannelPackerLibrary::UnpackTexture(const FChannelPackerUnpackRequest& Request)
{
    return TextureChannelPackerCore::Unpack(Request);
}

bool UTextureChannelPackerLibrary::MakePackRequestFromPreset(const FString& PresetName, FChannelPackerPackRequest& OutRequest)
{
    OutRequest = FChannelPackerPackRequest();

    FChannelPackerPreset Preset;
    if (!TextureChannelPackerCore::FindPreset(PresetName, Preset))
    {
        return false;
    }
    TextureChannelPackerCore::ApplyPresetToPackRequest(Preset, OutRequest);
    return true;
}

bool UTextureChannelPackerLibrary::MakeUnpackRequestFromPreset(const FString& PresetName, FChannelPackerUnpackRequest& OutRequest)
{
    OutRequest = FChannelPackerUnpackRequest();

    FChannelPackerPreset Preset;
    if (!TextureChannelPackerCore::FindPreset(PresetName, Preset))
    {
        return false;
    }
    TextureChannelPackerCore::ApplyPresetToUnpackRequest(Preset, OutRequest);
    return true;
}

TArray<FString> UTextureChannelPackerLibrary::GetPresetNames()
{
    TArray<FString> Names;
    for (const FChannelPackerPreset& Preset : TextureChannelPackerCore::GetAllPresets())
    {
        Names.Add(Preset.PresetName);
    }
    return Names;
}
