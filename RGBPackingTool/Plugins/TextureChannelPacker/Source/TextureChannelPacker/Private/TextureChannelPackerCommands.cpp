#include "TextureChannelPackerCommands.h"
#include "Styling/AppStyle.h"

#define LOCTEXT_NAMESPACE "FTextureChannelPackerModule"

FTextureChannelPackerCommands::FTextureChannelPackerCommands()
    : TCommands<FTextureChannelPackerCommands>(
        TEXT("TextureChannelPacker"),
        LOCTEXT("TextureChannelPackerCommandContext", "Texture Channel Packer"),
        NAME_None,
        FAppStyle::GetAppStyleSetName())
{
}

void FTextureChannelPackerCommands::RegisterCommands()
{
    // Ctrl+Alt+T (Cmd+Option+T on Mac). Users can change it under
    // Editor Preferences > Keyboard Shortcuts > Texture Channel Packer.
    UI_COMMAND(
        OpenPluginWindow,
        "Texture Channel Packer",
        "Opens the Texture Channel Packer tool.",
        EUserInterfaceActionType::Button,
        FInputChord(EModifierKey::Control | EModifierKey::Alt, EKeys::T));
}

#undef LOCTEXT_NAMESPACE
