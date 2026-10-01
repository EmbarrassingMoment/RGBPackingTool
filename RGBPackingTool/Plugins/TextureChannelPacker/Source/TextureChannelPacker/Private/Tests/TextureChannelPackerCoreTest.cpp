#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "TextureChannelPackerCore.h"
#include "TextureChannelPackerShared.h"
#include "Engine/Texture2D.h"
#include "TextureCompiler.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/StrongObjectPtr.h"

/**
 * Functional tests for the UI-independent pipeline (TextureChannelPackerCore): the code the
 * function library and the commandlet run. Uses tiny transient inputs and never saves to disk.
 *
 * Run headless with:
 *   UnrealEditor-Cmd.exe <Project.uproject> -ExecCmds="Automation RunTests TextureChannelPacker.Core" \
 *     -unattended -nopause -nosplash -nullrhi -testexit="Automation Test Queue Empty" -log
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTextureChannelPackerCorePackTest, "TextureChannelPacker.Core.Pack",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTextureChannelPackerCoreUnpackTest, "TextureChannelPacker.Core.Unpack",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTextureChannelPackerCoreValidationTest, "TextureChannelPacker.Core.Validation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace
{
    constexpr int32 TestSize = 4;
    constexpr int32 TestPixels = TestSize * TestSize;
    const TCHAR* const TestOutputPath = TEXT("/Game/TCPCoreTest");

    /** Creates a transient TestSize x TestSize BGRA8 texture; PixelFn gives the color of each pixel index. */
    UTexture2D* MakeBGRA8Texture(const TCHAR* Name, TFunctionRef<FColor(int32)> PixelFn)
    {
        UTexture2D* Texture = NewObject<UTexture2D>(GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), UTexture2D::StaticClass(), FName(Name)), RF_Transient);
        Texture->Source.Init(TestSize, TestSize, 1, 1, TSF_BGRA8);

        uint8* Data = Texture->Source.LockMip(0);
        if (Data)
        {
            for (int32 Index = 0; Index < TestPixels; ++Index)
            {
                const FColor Color = PixelFn(Index);
                Data[Index * 4 + 0] = Color.B;
                Data[Index * 4 + 1] = Color.G;
                Data[Index * 4 + 2] = Color.R;
                Data[Index * 4 + 3] = Color.A;
            }
        }
        Texture->Source.UnlockMip(0);
        return Texture;
    }

    UTexture2D* FindOutput(const FString& ObjectPath)
    {
        return FindObject<UTexture2D>(nullptr, *ObjectPath);
    }

    /** Releases output assets created by a test so reruns start clean. */
    void ReleaseOutputs(const TArray<FString>& ObjectPaths)
    {
        FTextureCompilingManager::Get().FinishAllCompilation();
        for (const FString& ObjectPath : ObjectPaths)
        {
            UTexture2D* Texture = FindOutput(ObjectPath);
            if (!Texture)
            {
                continue;
            }
            UPackage* Package = Texture->GetPackage();
            Texture->RemoveFromRoot();
            Texture->ClearFlags(RF_Standalone | RF_Public);
            Texture->MarkAsGarbage();
            Package->ClearFlags(RF_Standalone);
            Package->SetDirtyFlag(false);
            Package->MarkAsGarbage();
        }
        CollectGarbage(RF_NoFlags);
    }
}

bool FTextureChannelPackerCorePackTest::RunTest(const FString& Parameters)
{
    using namespace TextureChannelPackerCore;

    TStrongObjectPtr<UTexture2D> TexA(MakeBGRA8Texture(TEXT("T_CoreTest_A"), [](int32 i) { return FColor((uint8)(i * 16), 7, 200, (uint8)(255 - i)); }));
    TStrongObjectPtr<UTexture2D> TexB(MakeBGRA8Texture(TEXT("T_CoreTest_B"), [](int32 i) { return FColor(1, 2, 3, (uint8)(i * 8)); }));

    FChannelPackerPackRequest Request;
    Request.Red.Texture = TexA.Get();
    Request.Red.SourceChannel = EChannelPackerChannel::Red;
    Request.Green.Texture = TexB.Get();
    Request.Green.SourceChannel = EChannelPackerChannel::Alpha;
    Request.Green.bInvert = true;
    // Blue and Alpha stay empty: they must pack as 0 and 255.
    Request.OutputPath = TestOutputPath;
    Request.OutputName = TEXT("T_CoreTest_Packed");
    Request.bSave = false;

    const FChannelPackerResult Result = Pack(Request);
    TestEqual(TEXT("Pack succeeds"), StatusToString(Result.Status), FString(TEXT("Succeeded")));
    TestEqual(TEXT("Size 0x0 resolves to the input size (width)"), Result.Width, TestSize);
    TestEqual(TEXT("Size 0x0 resolves to the input size (height)"), Result.Height, TestSize);
    TestEqual(TEXT("No warnings"), Result.Warnings.Num(), 0);

    const FString ObjectPath = FString(TestOutputPath) + TEXT("/T_CoreTest_Packed.T_CoreTest_Packed");
    TestTrue(TEXT("Output object path is reported"), Result.OutputAssets.Num() == 1 && Result.OutputAssets[0] == ObjectPath);

    UTexture2D* Output = FindOutput(ObjectPath);
    if (TestNotNull(TEXT("Packed texture exists"), Output))
    {
        TestEqual(TEXT("Output is BGRA8"), (int32)Output->Source.GetFormat(), (int32)TSF_BGRA8);
        TestFalse(TEXT("Output is linear"), (bool)Output->SRGB);
        TestEqual(TEXT("Output uses Masks compression"), (int32)Output->CompressionSettings, (int32)TC_Masks);

        if (const uint8* Bytes = Output->Source.LockMipReadOnly(0))
        {
            bool bAllMatch = true;
            for (int32 i = 0; i < TestPixels; ++i)
            {
                // BGRA8 layout: [B, G, R, A]
                bAllMatch &= Bytes[i * 4 + 2] == (uint8)(i * 16);       // R <- A.Red
                bAllMatch &= Bytes[i * 4 + 1] == (uint8)(255 - i * 8);  // G <- inverted B.Alpha
                bAllMatch &= Bytes[i * 4 + 0] == 0;                     // empty Blue slot
                bAllMatch &= Bytes[i * 4 + 3] == 255;                   // empty Alpha slot
            }
            TestTrue(TEXT("Packed pixels match the inputs"), bAllMatch);
            Output->Source.UnlockMip(0);
        }
    }

    // Overwrite policies against the asset created above.
    FChannelPackerPackRequest Again = Request;
    Again.OverwritePolicy = EChannelPackerOverwritePolicy::Fail;
    const FChannelPackerResult FailResult = Pack(Again);
    TestEqual(TEXT("Fail policy reports an existing asset"), FailResult.ErrorCode, FString(TEXT("ErrorAssetExists")));

    Again.OverwritePolicy = EChannelPackerOverwritePolicy::Skip;
    TestEqual(TEXT("Skip policy skips an existing asset"), StatusToString(Pack(Again).Status), FString(TEXT("Skipped")));

    // Dry run: resolves the auto name and the explicit size, creates nothing.
    FChannelPackerPackRequest DryRun = Request;
    DryRun.OutputName.Empty();
    DryRun.FileNameSuffix = TEXT("_DryRunTest");
    DryRun.Width = 2;
    DryRun.Height = 2;
    DryRun.bDryRun = true;
    const FChannelPackerResult DryRunResult = Pack(DryRun);
    TestEqual(TEXT("Dry run status"), StatusToString(DryRunResult.Status), FString(TEXT("DryRun")));
    TestEqual(TEXT("Dry run keeps the explicit size"), DryRunResult.Width, 2);
    TestTrue(TEXT("Dry run reports the auto-generated name"), DryRunResult.OutputAssets.Num() == 1 && DryRunResult.OutputAssets[0].EndsWith(TEXT("_DryRunTest")));
    if (DryRunResult.OutputAssets.Num() == 1)
    {
        TestNull(TEXT("Dry run creates nothing"), FindOutput(DryRunResult.OutputAssets[0]));
    }

    ReleaseOutputs({ ObjectPath });
    return true;
}

bool FTextureChannelPackerCoreUnpackTest::RunTest(const FString& Parameters)
{
    using namespace TextureChannelPackerCore;

    // R and G vary; B (100) and A (255) are uniform.
    TStrongObjectPtr<UTexture2D> Source(MakeBGRA8Texture(TEXT("T_CoreTest_Source_ORM"), [](int32 i) { return FColor((uint8)(i * 16), (uint8)(255 - i), 100, 255); }));

    FChannelPackerUnpackRequest Request;
    Request.Source = Source.Get();
    Request.OutputPath = TestOutputPath;
    Request.BaseName = TEXT("T_CoreTest_Unpacked");
    Request.bSave = false;

    const FChannelPackerResult Result = Unpack(Request);
    TestEqual(TEXT("Unpack succeeds"), StatusToString(Result.Status), FString(TEXT("Succeeded")));
    TestEqual(TEXT("Uniform channels are skipped"), FString::Join(Result.SkippedUniformChannels, TEXT(",")), FString(TEXT("B,A")));
    TestEqual(TEXT("Two outputs"), Result.OutputAssets.Num(), 2);

    const FString RedPath = FString(TestOutputPath) + TEXT("/T_CoreTest_Unpacked_R.T_CoreTest_Unpacked_R");
    const FString GreenPath = FString(TestOutputPath) + TEXT("/T_CoreTest_Unpacked_G.T_CoreTest_Unpacked_G");

    struct FExpectedChannel
    {
        const FString* ObjectPath;
        TFunction<uint8(int32)> Value;
    };
    const FExpectedChannel Expected[] =
    {
        { &RedPath,   [](int32 i) { return (uint8)(i * 16); } },
        { &GreenPath, [](int32 i) { return (uint8)(255 - i); } },
    };

    for (const FExpectedChannel& Channel : Expected)
    {
        UTexture2D* Output = FindOutput(*Channel.ObjectPath);
        if (!TestNotNull(FString::Printf(TEXT("%s exists"), **Channel.ObjectPath), Output))
        {
            continue;
        }
        TestEqual(TEXT("Output is G8"), (int32)Output->Source.GetFormat(), (int32)TSF_G8);
        TestEqual(TEXT("Output uses Grayscale compression"), (int32)Output->CompressionSettings, (int32)TC_Grayscale);
        if (const uint8* Bytes = Output->Source.LockMipReadOnly(0))
        {
            bool bAllMatch = true;
            for (int32 i = 0; i < TestPixels; ++i)
            {
                bAllMatch &= Bytes[i] == Channel.Value(i);
            }
            TestTrue(FString::Printf(TEXT("%s pixels match the source channel"), **Channel.ObjectPath), bAllMatch);
            Output->Source.UnlockMip(0);
        }
    }

    // Dry run of every channel (uniform ones included) with preset suffixes; creates nothing.
    FChannelPackerUnpackRequest DryRun = Request;
    DryRun.bSkipUniformChannels = false;
    DryRun.SuffixBlue = TEXT("_Metallic");
    DryRun.OverwritePolicy = EChannelPackerOverwritePolicy::Overwrite;
    DryRun.bDryRun = true;
    const FChannelPackerResult DryRunResult = Unpack(DryRun);
    TestEqual(TEXT("Dry run status"), StatusToString(DryRunResult.Status), FString(TEXT("DryRun")));
    TestEqual(TEXT("Dry run lists four outputs"), DryRunResult.OutputAssets.Num(), 4);
    TestNull(TEXT("Dry run creates nothing"), FindOutput(FString(TestOutputPath) + TEXT("/T_CoreTest_Unpacked_Metallic.T_CoreTest_Unpacked_Metallic")));

    // Only uniform channels requested: nothing to export.
    FChannelPackerUnpackRequest UniformOnly = Request;
    UniformOnly.bExportRed = false;
    UniformOnly.bExportGreen = false;
    TestEqual(TEXT("Only uniform channels -> ErrorNoChannelsToExport"), Unpack(UniformOnly).ErrorCode, FString(TEXT("ErrorNoChannelsToExport")));

    ReleaseOutputs({ RedPath, GreenPath });
    return true;
}

bool FTextureChannelPackerCoreValidationTest::RunTest(const FString& Parameters)
{
    using namespace TextureChannelPackerCore;

    TStrongObjectPtr<UTexture2D> Input(MakeBGRA8Texture(TEXT("T_CoreTest_Input"), [](int32 i) { return FColor((uint8)i, 0, 0, 255); }));

    auto MakeValidPack = [&Input]()
    {
        FChannelPackerPackRequest Request;
        Request.Red.Texture = Input.Get();
        Request.OutputPath = TestOutputPath;
        Request.OutputName = TEXT("T_CoreTest_NeverCreated");
        Request.bSave = false;
        Request.bDryRun = true;
        return Request;
    };

    TestEqual(TEXT("A valid dry run passes"), StatusToString(Pack(MakeValidPack()).Status), FString(TEXT("DryRun")));

    {
        FChannelPackerPackRequest Request = MakeValidPack();
        Request.Red.Texture.Reset();
        TestEqual(TEXT("No inputs"), Pack(Request).ErrorCode, FString(TEXT("ErrorNoTextures")));
    }
    {
        FChannelPackerPackRequest Request = MakeValidPack();
        Request.Width = 8;
        Request.Height = 0;
        TestEqual(TEXT("Only one dimension set"), Pack(Request).ErrorCode, FString(TEXT("ErrorInvalidResolution")));
    }
    {
        FChannelPackerPackRequest Request = MakeValidPack();
        Request.Green.Texture = TSoftObjectPtr<UTexture2D>(FSoftObjectPath(TEXT("/Game/TCPCoreTest/DoesNotExist/T_Missing.T_Missing")));
        TestEqual(TEXT("Missing input"), Pack(Request).ErrorCode, FString(TEXT("ErrorInputNotFound")));
    }
    {
        FChannelPackerPackRequest Request = MakeValidPack();
        Request.Compression = TEXT("BC7");
        TestEqual(TEXT("Unknown compression"), Pack(Request).ErrorCode, FString(TEXT("ErrorInvalidCompression")));
    }
    {
        FChannelPackerPackRequest Request = MakeValidPack();
        Request.OutputPath = TEXT("Game/NoLeadingSlash");
        TestEqual(TEXT("Output path must be a content path"), Pack(Request).ErrorCode, FString(TEXT("ErrorInvalidOutputPath")));
    }
    {
        FChannelPackerPackRequest Request = MakeValidPack();
        Request.OutputName = TEXT("T Has Spaces");
        TestEqual(TEXT("Output name must be a valid object name"), Pack(Request).ErrorCode, FString(TEXT("ErrorInvalidOutputName")));
    }
    {
        FChannelPackerUnpackRequest Request;
        Request.Source = Input.Get();
        Request.bExportRed = Request.bExportGreen = Request.bExportBlue = Request.bExportAlpha = false;
        TestEqual(TEXT("Unpack without channels"), Unpack(Request).ErrorCode, FString(TEXT("ErrorNoChannelsSelected")));
    }
    {
        FChannelPackerUnpackRequest Request;
        TestEqual(TEXT("Unpack without source"), Unpack(Request).ErrorCode, FString(TEXT("ErrorNoUnpackSource")));
    }

    // Naming helpers (same rules as the editor tool).
    TestEqual(TEXT("Packed name uses the common prefix"), MakePackedAssetName({ TEXT("T_Rock_AO"), TEXT("T_Rock_Roughness") }, TEXT("_ORM")), FString(TEXT("T_Rock_ORM")));
    TestEqual(TEXT("Packed name enforces T_"), MakePackedAssetName({ TEXT("Rock_AO") }, TEXT("_MRA")), FString(TEXT("T_Rock_AO_MRA")));
    TestEqual(TEXT("Unpack base strips a known suffix"), MakeUnpackBaseName(TEXT("T_Rock_ORM"), { TEXT("_ORM"), TEXT("_MRA") }), FString(TEXT("T_Rock")));
    TestEqual(TEXT("Unpack base suffix match ignores case"), MakeUnpackBaseName(TEXT("Rock_packed"), { TEXT("_Packed") }), FString(TEXT("T_Rock")));

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
