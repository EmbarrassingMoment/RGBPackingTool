#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "TextureChannelPackerCommandlet.generated.h"

/**
 * @class UTextureChannelPackerCommandlet
 * @brief Runs a JSON file of Pack / Unpack jobs headlessly and writes a JSON result file.
 *
 * UnrealEditor-Cmd <Project>.uproject -run=TextureChannelPacker -Job=<jobs.json> [-Result=<result.json>] [-DryRun] -unattended -nullrhi
 *
 * Relative paths are resolved against the directory the process was launched from. The job
 * and result schema is documented in Docs/Headless.md.
 *
 * Exit codes: 0 = every job succeeded (Skipped and DryRun count as success),
 *             1 = bad arguments or unreadable job file, 2 = at least one job failed.
 */
UCLASS()
class UTextureChannelPackerCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:
    UTextureChannelPackerCommandlet();

    virtual int32 Main(const FString& Params) override;
};
