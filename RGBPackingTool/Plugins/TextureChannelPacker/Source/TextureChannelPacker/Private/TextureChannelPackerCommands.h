#pragma once

#include "CoreMinimal.h"
#include "Framework/Commands/Commands.h"

/**
 * @class FTextureChannelPackerCommands
 * @brief Editor commands of the Texture Channel Packer plugin.
 *
 * Declaring the "open the tool" action as a command (instead of a bare menu lambda) gives it a
 * keyboard shortcut, shows that shortcut next to the menu entry, and lists it under
 * Editor Preferences > Keyboard Shortcuts so users can rebind it.
 */
class FTextureChannelPackerCommands : public TCommands<FTextureChannelPackerCommands>
{
public:
    FTextureChannelPackerCommands();

    virtual void RegisterCommands() override;

    /** Opens (or focuses) the Texture Channel Packer tab. */
    TSharedPtr<FUICommandInfo> OpenPluginWindow;
};
