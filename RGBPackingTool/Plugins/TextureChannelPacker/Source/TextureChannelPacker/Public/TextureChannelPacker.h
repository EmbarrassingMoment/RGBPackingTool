#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class SDockTab;
class FSpawnTabArgs;
class UTexture2D;

class FTextureChannelPackerModule : public IModuleInterface
{
public:
    /** IModuleInterface implementation */
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    /** Callback for spawning the plugin tab */
    TSharedRef<SDockTab> OnSpawnPluginTab(const FSpawnTabArgs& SpawnTabArgs);

    /** Input textures for each channel */
    TWeakObjectPtr<UTexture2D> InputTextureR;
    TWeakObjectPtr<UTexture2D> InputTextureG;
    TWeakObjectPtr<UTexture2D> InputTextureB;
};
