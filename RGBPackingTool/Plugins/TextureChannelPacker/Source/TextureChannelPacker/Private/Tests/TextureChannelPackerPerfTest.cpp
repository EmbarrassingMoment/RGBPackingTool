#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "TextureChannelPacker.h"
#include "TextureChannelPackerShared.h"
#include "Engine/Texture2D.h"
#include "TextureCompiler.h"
#include "Modules/ModuleManager.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/StrongObjectPtr.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Math/Float16.h"

using namespace TextureChannelPackerUtils;

/**
 * Performance benchmark for the Pack pipeline.
 *
 * Builds synthetic source textures of representative sizes and formats, then drives the
 * module's real CreateTexture() and UpdatePreview() code paths, timing each run. Per-phase
 * timings are logged by the FPhaseTimer instances inside those functions; this test adds
 * the end-to-end wall-clock time per scenario and a min/median/max summary.
 *
 * Run headless with:
 *   UnrealEditor-Cmd.exe <Project.uproject> -ExecCmds="Automation RunTests TextureChannelPacker.Perf" \
 *     -unattended -nopause -nosplash -nullrhi -testexit="Automation Test Queue Empty" -log
 *
 * Iterations default to 3 and can be overridden with -TCPPerfIterations=N.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTextureChannelPackerPerfTest, "TextureChannelPacker.Perf.Pack",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::PerfFilter)

namespace
{
    struct FPerfScenario
    {
        const TCHAR* Name;
        ETextureSourceFormat InputFormat;
        int32 SrcSize;
        int32 NumInputs;   // 4 = all slots, 1 = R slot only
        int32 TargetSize;
        bool bPreview;     // true = UpdatePreview(), false = CreateTexture()
    };

    /** Creates a transient UTexture2D with deterministic, non-uniform source data. */
    UTexture2D* MakeSourceTexture(const TCHAR* Name, int32 Size, ETextureSourceFormat Format, uint32 Seed)
    {
        UTexture2D* Texture = NewObject<UTexture2D>(GetTransientPackage(), FName(Name), RF_Transient);
        Texture->Source.Init(Size, Size, 1, 1, Format);

        uint8* Data = Texture->Source.LockMip(0);
        if (!Data)
        {
            return nullptr;
        }

        const int64 NumPixels = (int64)Size * (int64)Size;
        switch (Format)
        {
        case TSF_BGRA8:
        {
            const int64 NumBytes = NumPixels * 4;
            for (int64 i = 0; i < NumBytes; ++i)
            {
                Data[i] = (uint8)((i * 31 + Seed) ^ (i >> 7));
            }
            break;
        }
        case TSF_G8:
        {
            for (int64 i = 0; i < NumPixels; ++i)
            {
                Data[i] = (uint8)((i * 31 + Seed) ^ (i >> 7));
            }
            break;
        }
        case TSF_G16:
        {
            uint16* Pixels = (uint16*)Data;
            for (int64 i = 0; i < NumPixels; ++i)
            {
                Pixels[i] = (uint16)((i * 31 + Seed) ^ (i >> 5));
            }
            break;
        }
        case TSF_R32F:
        {
            float* Pixels = (float*)Data;
            for (int64 i = 0; i < NumPixels; ++i)
            {
                Pixels[i] = (float)(((i * 31 + Seed) ^ (i >> 7)) & 255) / 255.0f;
            }
            break;
        }
        case TSF_RGBA32F:
        {
            float* Pixels = (float*)Data;
            const int64 NumFloats = NumPixels * 4;
            for (int64 i = 0; i < NumFloats; ++i)
            {
                Pixels[i] = (float)(((i * 31 + Seed) ^ (i >> 7)) & 255) / 255.0f;
            }
            break;
        }
        default:
            break;
        }
        Texture->Source.UnlockMip(0);
        return Texture;
    }

    /** Releases a packed output asset created by CreateTexture() so iterations do not accumulate memory. */
    void ReleasePackedAsset(const FString& PackageName)
    {
        UPackage* Package = FindPackage(nullptr, *PackageName);
        if (!Package)
        {
            return;
        }
        const FString AssetName = FPaths::GetBaseFilename(PackageName);
        if (UTexture2D* Texture = FindObject<UTexture2D>(Package, *AssetName))
        {
            Texture->RemoveFromRoot();
            Texture->ClearFlags(RF_Standalone | RF_Public);
            Texture->MarkAsGarbage();
        }
        Package->ClearFlags(RF_Standalone);
        Package->SetDirtyFlag(false);
        Package->MarkAsGarbage();
    }

    double Median(TArray<double> Values)
    {
        Values.Sort();
        const int32 N = Values.Num();
        if (N == 0)
        {
            return 0.0;
        }
        return (N % 2 == 1) ? Values[N / 2] : 0.5 * (Values[N / 2 - 1] + Values[N / 2]);
    }
}

bool FTextureChannelPackerPerfTest::RunTest(const FString& Parameters)
{
    int32 Iterations = 3;
    FParse::Value(FCommandLine::Get(), TEXT("TCPPerfIterations="), Iterations);
    Iterations = FMath::Clamp(Iterations, 1, 20);

    static const FPerfScenario Scenarios[] =
    {
        { TEXT("Pack_BGRA8_4096_same"),        TSF_BGRA8,   4096, 4, 4096, false },
        { TEXT("Pack_BGRA8_8192_same"),        TSF_BGRA8,   8192, 4, 8192, false },
        { TEXT("Pack_BGRA8_8192_to_4096"),     TSF_BGRA8,   8192, 4, 4096, false },
        { TEXT("Pack_G8_8192_to_4096"),        TSF_G8,      8192, 4, 4096, false },
        { TEXT("Pack_RGBA32F_4096_to_2048"),   TSF_RGBA32F, 4096, 1, 2048, false },
        { TEXT("Preview_BGRA8_8192"),          TSF_BGRA8,   8192, 4, 8192, true  },
    };

    FTextureChannelPackerModule& Module = FModuleManager::GetModuleChecked<FTextureChannelPackerModule>(TEXT("TextureChannelPacker"));

    UE_LOG(LogTexturePacker, Log, TEXT("[PerfSummary] ==== TextureChannelPacker Pack benchmark (%d iteration(s)) ===="), Iterations);

    for (const FPerfScenario& Scenario : Scenarios)
    {
        // Build inputs (not timed).
        TStrongObjectPtr<UTexture2D> Inputs[4];
        for (int32 Slot = 0; Slot < Scenario.NumInputs; ++Slot)
        {
            const FString TexName = FString::Printf(TEXT("PerfInput_%s_%d"), Scenario.Name, Slot);
            UTexture2D* Tex = MakeSourceTexture(*TexName, Scenario.SrcSize, Scenario.InputFormat, 17u * (Slot + 1));
            if (!Tex)
            {
                AddError(FString::Printf(TEXT("Failed to create source texture for %s"), Scenario.Name));
                return false;
            }
            Inputs[Slot].Reset(Tex);
        }

        Module.InputTextureR = Inputs[0].Get();
        Module.InputTextureG = Scenario.NumInputs > 1 ? Inputs[1].Get() : nullptr;
        Module.InputTextureB = Scenario.NumInputs > 2 ? Inputs[2].Get() : nullptr;
        Module.InputTextureA = Scenario.NumInputs > 3 ? Inputs[3].Get() : nullptr;
        Module.SourceChannelR = ESourceChannel::Red;
        Module.SourceChannelG = ESourceChannel::Green;
        Module.SourceChannelB = ESourceChannel::Blue;
        Module.SourceChannelA = ESourceChannel::Alpha;
        Module.bInvertR = Module.bInvertG = Module.bInvertB = Module.bInvertA = false;
        Module.TargetWidth = Scenario.TargetSize;
        Module.TargetHeight = Scenario.TargetSize;

        TArray<double> Timings;
        for (int32 Iter = 0; Iter < Iterations; ++Iter)
        {
            UE_LOG(LogTexturePacker, Log, TEXT("[Perf] --- %s iteration %d ---"), Scenario.Name, Iter + 1);

            const double Start = FPlatformTime::Seconds();
            FString PackageName;
            if (Scenario.bPreview)
            {
                Module.UpdatePreview();
            }
            else
            {
                PackageName = FString::Printf(TEXT("/Game/TCPPerf/%s_%d"), Scenario.Name, Iter);
                Module.CreateTexture(PackageName, Scenario.TargetSize, Scenario.TargetSize);
            }
            const double Elapsed = (FPlatformTime::Seconds() - Start) * 1000.0;
            Timings.Add(Elapsed);

            UE_LOG(LogTexturePacker, Log, TEXT("[Perf] %s iteration %d: %.1f ms"), Scenario.Name, Iter + 1, Elapsed);

            if (!Scenario.bPreview)
            {
                // Sanity check on the single-input scenario: the empty Alpha slot must pack as
                // opaque white (255), and the empty G/B slots as black (0).
                if (Scenario.NumInputs == 1)
                {
                    UPackage* OutPackage = FindPackage(nullptr, *PackageName);
                    UTexture2D* OutTexture = OutPackage ? FindObject<UTexture2D>(OutPackage, *FPaths::GetBaseFilename(PackageName)) : nullptr;
                    if (!OutTexture)
                    {
                        AddError(FString::Printf(TEXT("%s: packed texture not found"), Scenario.Name));
                    }
                    else if (const uint8* OutBytes = OutTexture->Source.LockMipReadOnly(0))
                    {
                        // BGRA8 layout: [B, G, R, A]
                        TestEqual(FString::Printf(TEXT("%s: empty Blue slot packs as 0"), Scenario.Name), (int32)OutBytes[0], 0);
                        TestEqual(FString::Printf(TEXT("%s: empty Green slot packs as 0"), Scenario.Name), (int32)OutBytes[1], 0);
                        TestEqual(FString::Printf(TEXT("%s: empty Alpha slot packs as 255"), Scenario.Name), (int32)OutBytes[3], 255);
                        OutTexture->Source.UnlockMip(0);
                    }
                }

                // Drain the engine's async texture build (mips + compression) so it does not
                // overlap with the next iteration. This is engine work and is reported separately.
                const double BuildStart = FPlatformTime::Seconds();
                FTextureCompilingManager::Get().FinishAllCompilation();
                UE_LOG(LogTexturePacker, Log, TEXT("[Perf] %s engine texture build (async, excluded): %.1f ms"),
                    Scenario.Name, (FPlatformTime::Seconds() - BuildStart) * 1000.0);

                ReleasePackedAsset(PackageName);
                CollectGarbage(RF_NoFlags);
            }
        }

        const double Min = FMath::Min(Timings);
        const double Max = FMath::Max(Timings);
        const double Med = Median(Timings);
        const FString Line = FString::Printf(TEXT("[PerfSummary] %-32s min %8.1f ms  median %8.1f ms  max %8.1f ms"), Scenario.Name, Min, Med, Max);
        UE_LOG(LogTexturePacker, Log, TEXT("%s"), *Line);
        AddInfo(Line);

        // Release inputs before the next scenario.
        Module.InputTextureR = nullptr;
        Module.InputTextureG = nullptr;
        Module.InputTextureB = nullptr;
        Module.InputTextureA = nullptr;
        for (TStrongObjectPtr<UTexture2D>& Input : Inputs)
        {
            Input.Reset();
        }
        CollectGarbage(RF_NoFlags);
    }

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
