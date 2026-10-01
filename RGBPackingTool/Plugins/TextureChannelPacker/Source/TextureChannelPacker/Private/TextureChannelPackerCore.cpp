#include "TextureChannelPackerCore.h"
#include "TextureChannelPackerShared.h"
#include "Engine/Texture2D.h"
#include "TextureCompiler.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Async/ParallelFor.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopedSlowTask.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"
#include <atomic>

using namespace TextureChannelPackerUtils;

namespace TextureChannelPackerCore
{

namespace
{
    /** Channel letters, indexed R=0, G=1, B=2, A=3. */
    const TCHAR* const GChannelLetters[4] = { TEXT("R"), TEXT("G"), TEXT("B"), TEXT("A") };

    /** Marks Result as failed with a stable code and a localized message. */
    FChannelPackerResult& SetFailed(FChannelPackerResult& Result, const TCHAR* Code, const FText& Message)
    {
        Result.Status = EChannelPackerStatus::Failed;
        Result.ErrorCode = Code;
        Result.Message = Message.ToString();
        UE_LOG(LogTexturePacker, Log, TEXT("Request failed [%s]: %s"), Code, *Result.Message);
        return Result;
    }

    void AddWarning(FChannelPackerResult& Result, const FString& Code, const FString& Channel, const FText& Message)
    {
        FChannelPackerWarning& Warning = Result.Warnings.AddDefaulted_GetRef();
        Warning.Code = Code;
        Warning.Channel = Channel;
        Warning.Message = Message.ToString();
        UE_LOG(LogTexturePacker, Warning, TEXT("[%s] (%s) %s"), *Code, *Channel, *Warning.Message);
    }

    /** Joins a content folder and an asset name into a long package name ("/Game/Folder" + "T_X" -> "/Game/Folder/T_X"). */
    FString JoinPackageName(const FString& OutputPath, const FString& AssetName)
    {
        FString PackagePath = OutputPath.TrimStartAndEnd();
        while (PackagePath.EndsWith(TEXT("/")))
        {
            PackagePath.LeftChopInline(1);
        }
        return PackagePath + TEXT("/") + AssetName;
    }

    bool ContainsAnyChar(const FString& Text, const TCHAR* Chars)
    {
        int32 Unused = INDEX_NONE;
        for (const TCHAR* Char = Chars; *Char; ++Char)
        {
            if (Text.FindChar(*Char, Unused))
            {
                return true;
            }
        }
        return false;
    }

    /** Rejects names that would produce a broken package or object. */
    bool ValidateOutputName(const FString& PackageName, const FString& AssetName, FChannelPackerResult& Result)
    {
        if (AssetName.IsEmpty() || ContainsAnyChar(AssetName, INVALID_OBJECTNAME_CHARACTERS))
        {
            SetFailed(Result, TEXT("ErrorInvalidOutputName"), FText::Format(
                GetLocalizedMessage(TEXT("ErrorInvalidOutputName"),
                    TEXT("Invalid output asset name: \"{0}\"."),
                    TEXT("出力アセット名が不正です: \"{0}\"")),
                FText::FromString(AssetName)));
            return false;
        }
        if (!FPackageName::IsValidLongPackageName(PackageName))
        {
            SetFailed(Result, TEXT("ErrorInvalidOutputPath"), FText::Format(
                GetLocalizedMessage(TEXT("ErrorInvalidOutputPath"),
                    TEXT("Invalid output package: \"{0}\". The output path must be a content folder such as /Game/Textures."),
                    TEXT("出力パッケージが不正です: \"{0}\"。出力パスには /Game/Textures のようなコンテンツフォルダを指定してください。")),
                FText::FromString(PackageName)));
            return false;
        }
        return true;
    }

    /**
     * Resolves an input texture. Checks that the package exists before loading, so a wrong path
     * fails cleanly instead of making the loader log warnings.
     */
    UTexture2D* ResolveTexture(const TSoftObjectPtr<UTexture2D>& SoftTexture)
    {
        if (UTexture2D* Loaded = SoftTexture.Get())
        {
            return Loaded;
        }
        if (!FPackageName::DoesPackageExist(SoftTexture.ToSoftObjectPath().GetLongPackageName()))
        {
            return nullptr;
        }
        return SoftTexture.LoadSynchronous();
    }

    /** True if the asset exists on disk or was created earlier in this session (not saved yet). */
    bool DoesAssetExist(const FString& PackageName, const FString& AssetName)
    {
        if (FPackageName::DoesPackageExist(PackageName))
        {
            return true;
        }
        const UObject* InMemory = FindObject<UObject>(nullptr, *(PackageName + TEXT(".") + AssetName));
        return IsValid(InMemory);
    }

    /**
     * Creates (or loads) the output package and makes sure an existing object with the asset's
     * name is a texture: NewObject would otherwise try to replace an object of another class.
     */
    UPackage* PrepareOutputPackage(const FString& PackageName, const FString& AssetName, FString& OutErrorCode, FText& OutErrorMessage)
    {
        UPackage* Package = CreatePackage(*PackageName);
        if (!Package)
        {
            OutErrorCode = TEXT("ErrorPackageCreation");
            OutErrorMessage = GetLocalizedMessage(
                TEXT("ErrorPackageCreation"),
                TEXT("Failed to create package."),
                TEXT("パッケージの作成に失敗しました。"));
            return nullptr;
        }

        // Loads an existing asset so it is replaced in place rather than duplicated.
        Package->FullyLoad();

        if (const UObject* Existing = FindObject<UObject>(Package, *AssetName))
        {
            if (!Existing->IsA<UTexture2D>())
            {
                OutErrorCode = TEXT("ErrorAssetTypeMismatch");
                OutErrorMessage = FText::Format(
                    GetLocalizedMessage(TEXT("ErrorAssetTypeMismatch"),
                        TEXT("{0} already exists and is not a Texture2D ({1}). Choose another output name."),
                        TEXT("{0} は既に存在し、Texture2D ではありません ({1})。別の出力名を指定してください。")),
                    FText::FromString(PackageName),
                    FText::FromString(Existing->GetClass()->GetName()));
                return nullptr;
            }
        }
        return Package;
    }

    /** Saves the package that holds Asset. Waits for the async texture build first so the saved asset is complete. */
    bool SaveAsset(UPackage* Package, UObject* Asset)
    {
        if (UTexture* Texture = Cast<UTexture>(Asset))
        {
            FTextureCompilingManager::Get().FinishCompilation({ Texture });
        }

        const FString Filename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());

        FSavePackageArgs SaveArgs;
        SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
        SaveArgs.SaveFlags = SAVE_NoError;
        const bool bSaved = UPackage::SavePackage(Package, Asset, *Filename, SaveArgs);
        UE_LOG(LogTexturePacker, Log, TEXT("%s %s"), bSaved ? TEXT("Saved") : TEXT("Failed to save"), *Filename);
        return bSaved;
    }

    FText MakeSaveFailedMessage(const FString& PackageName)
    {
        return FText::Format(
            GetLocalizedMessage(TEXT("ErrorSaveFailed"),
                TEXT("Failed to save {0}. Check that the file is writable (e.g. checked out from source control)."),
                TEXT("{0} の保存に失敗しました。ファイルが書き込み可能か（ソース管理でチェックアウト済みか等）確認してください。")),
            FText::FromString(PackageName));
    }

    /**
     * @brief Detects channels whose pixels all hold one value, reading the source at full resolution.
     *
     * Each channel scan stops at the first differing pixel, so a used channel costs little.
     */
    template <typename FSampler>
    void DetectUniformChannels(const FSampler& Sample, int32 Width, int32 Height, const bool (&bChannels)[4], bool (&bOutUniform)[4])
    {
        for (int32 Channel = 0; Channel < 4; ++Channel)
        {
            bOutUniform[Channel] = false;
            if (!bChannels[Channel])
            {
                continue;
            }

            const uint8 First = Sample(0, Channel);
            std::atomic<bool> bDiffers(false);
            ParallelFor(Height, [&](int32 Y)
            {
                if (bDiffers.load(std::memory_order_relaxed))
                {
                    return;
                }
                const int64 RowStart = (int64)Y * Width;
                for (int32 X = 0; X < Width; ++X)
                {
                    if (Sample(RowStart + X, Channel) != First)
                    {
                        bDiffers.store(true, std::memory_order_relaxed);
                        return;
                    }
                }
            });
            bOutUniform[Channel] = !bDiffers.load();
        }
    }

    /**
     * @brief Copies one channel out of the source at full resolution.
     *
     * Reads straight from the locked mip through the sampler, so the only allocation is the
     * output buffer itself (one byte per pixel).
     */
    template <typename FSampler>
    void ExtractChannelBytes(const FSampler& Sample, int32 Channel, int64 NumPixels, TArray<uint8>& Out)
    {
        Out.SetNumUninitialized((int32)NumPixels);
        uint8* Dest = Out.GetData();

        ParallelFor((int32)NumPixels, [&Sample, Dest, Channel](int32 PixelIndex)
        {
            Dest[PixelIndex] = Sample((int64)PixelIndex, Channel);
        });
    }

    // ========== Preset Persistence ==========

    FString GetPresetsDirectory()
    {
        return FPaths::ProjectSavedDir() / TEXT("TextureChannelPacker") / TEXT("Presets");
    }

    FString SanitizePresetFileName(const FString& Name)
    {
        FString Sanitized = Name;
        // Remove characters that are invalid in filenames
        Sanitized = Sanitized.Replace(TEXT("/"), TEXT("_"));
        Sanitized = Sanitized.Replace(TEXT("\\"), TEXT("_"));
        Sanitized = Sanitized.Replace(TEXT(":"), TEXT("_"));
        Sanitized = Sanitized.Replace(TEXT("*"), TEXT("_"));
        Sanitized = Sanitized.Replace(TEXT("?"), TEXT("_"));
        Sanitized = Sanitized.Replace(TEXT("\""), TEXT("_"));
        Sanitized = Sanitized.Replace(TEXT("<"), TEXT("_"));
        Sanitized = Sanitized.Replace(TEXT(">"), TEXT("_"));
        Sanitized = Sanitized.Replace(TEXT("|"), TEXT("_"));
        return Sanitized;
    }
}

// ========== Pack ==========

FChannelPackerResult Pack(const FChannelPackerPackRequest& Request, const FExecutionOptions& Options)
{
    check(IsInGameThread());

    FPhaseTimer TotalTimer(TEXT("Pack total"));
    FChannelPackerResult Result;

#if WITH_EDITORONLY_DATA
    const FChannelPackerInput* Inputs[4] = { &Request.Red, &Request.Green, &Request.Blue, &Request.Alpha };

    // ---------------------------------------------------------
    // Validate and resolve the request (nothing is read or written yet)
    // ---------------------------------------------------------
    UTexture2D* InputTextures[4] = { nullptr, nullptr, nullptr, nullptr };
    TArray<FString> InputNames;
    for (int32 Index = 0; Index < 4; ++Index)
    {
        const TSoftObjectPtr<UTexture2D>& SoftTexture = Inputs[Index]->Texture;
        if (SoftTexture.IsNull())
        {
            continue;
        }

        InputTextures[Index] = ResolveTexture(SoftTexture);
        if (!InputTextures[Index])
        {
            return SetFailed(Result, TEXT("ErrorInputNotFound"), FText::Format(
                GetLocalizedMessage(TEXT("ErrorInputNotFound"),
                    TEXT("Input texture not found (or not a Texture2D): {0}"),
                    TEXT("入力テクスチャが見つからないか、Texture2D ではありません: {0}")),
                FText::FromString(SoftTexture.ToString())));
        }
        InputNames.Add(InputTextures[Index]->GetName());
    }

    if (InputNames.Num() == 0)
    {
        return SetFailed(Result, TEXT("ErrorNoTextures"), GetLocalizedMessage(
            TEXT("ErrorNoTextures"),
            TEXT("Please select at least one input texture."),
            TEXT("入力テクスチャを少なくとも1つ選択してください。")));
    }

    int32 Width = Request.Width;
    int32 Height = Request.Height;
    if (Width == 0 && Height == 0)
    {
        // Match the largest input (by pixel count) so nothing is downscaled by default.
        int64 LargestPixels = 0;
        for (UTexture2D* Texture : InputTextures)
        {
            if (!Texture)
            {
                continue;
            }
            const int32 SrcWidth = Texture->Source.GetSizeX();
            const int32 SrcHeight = Texture->Source.GetSizeY();
            if ((int64)SrcWidth * (int64)SrcHeight > LargestPixels)
            {
                LargestPixels = (int64)SrcWidth * (int64)SrcHeight;
                Width = SrcWidth;
                Height = SrcHeight;
            }
        }
    }

    if (Width < 1 || Width > MaxTextureDimension || Height < 1 || Height > MaxTextureDimension)
    {
        return SetFailed(Result, TEXT("ErrorInvalidResolution"), FText::Format(
            GetLocalizedMessage(TEXT("ErrorInvalidResolution"),
                TEXT("Width and Height must each be between 1 and {0}."),
                TEXT("幅と高さはそれぞれ 1 から {0} の間で指定してください。")),
            FText::AsNumber(MaxTextureDimension)));
    }
    Result.Width = Width;
    Result.Height = Height;

    FString AssetName = Request.OutputName.TrimStartAndEnd();
    if (AssetName.IsEmpty())
    {
        AssetName = MakePackedAssetName(InputNames, Request.FileNameSuffix);
    }
    const FString PackageName = JoinPackageName(Request.OutputPath, AssetName);
    if (!ValidateOutputName(PackageName, AssetName, Result))
    {
        return Result;
    }

    TextureCompressionSettings CompressionSetting = TC_Masks;
    if (!FindCompressionSetting(Request.Compression, CompressionSetting))
    {
        return SetFailed(Result, TEXT("ErrorInvalidCompression"), FText::Format(
            GetLocalizedMessage(TEXT("ErrorInvalidCompression"),
                TEXT("Unknown compression option: \"{0}\". Expected Masks, Grayscale or Default."),
                TEXT("不明な圧縮設定です: \"{0}\"。Masks / Grayscale / Default のいずれかを指定してください。")),
            FText::FromString(Request.Compression)));
    }

    Result.OutputAssets.Add(PackageName + TEXT(".") + AssetName);

    if (Request.OverwritePolicy != EChannelPackerOverwritePolicy::Overwrite && DoesAssetExist(PackageName, AssetName))
    {
        if (Request.OverwritePolicy == EChannelPackerOverwritePolicy::Skip)
        {
            Result.Status = EChannelPackerStatus::Skipped;
            Result.Message = FText::Format(
                GetLocalizedMessage(TEXT("SkippedAssetExists"),
                    TEXT("{0} already exists; skipped."),
                    TEXT("{0} は既に存在するためスキップしました。")),
                FText::FromString(PackageName)).ToString();
            return Result;
        }
        return SetFailed(Result, TEXT("ErrorAssetExists"), FText::Format(
            GetLocalizedMessage(TEXT("ErrorAssetExists"),
                TEXT("{0} already exists. Use the Overwrite or Skip overwrite policy."),
                TEXT("{0} は既に存在します。上書きポリシーに Overwrite または Skip を指定してください。")),
            FText::FromString(PackageName)));
    }

    if (Request.bDryRun)
    {
        Result.Status = EChannelPackerStatus::DryRun;
        Result.Message = FText::Format(
            GetLocalizedMessage(TEXT("DryRunPack"),
                TEXT("Dry run: would create {0} ({1} x {2})."),
                TEXT("ドライラン: {0} ({1} x {2}) を作成します。")),
            FText::FromString(PackageName), FText::AsNumber(Width), FText::AsNumber(Height)).ToString();
        return Result;
    }

    // ---------------------------------------------------------
    // Process
    // ---------------------------------------------------------
    FScopedSlowTask SlowTask(5.0f, GetLocalizedMessage(
        TEXT("ProgressProcessing"),
        TEXT("Processing Textures..."),
        TEXT("テクスチャを処理中...")
    ));
    if (Options.bShowProgressDialog)
    {
        SlowTask.MakeDialog(true); // true = cancellable
    }

    // Nothing has been written when any of the cancellation points below is reached.
    auto MakeCancelledResult = [&Result]()
    {
        Result.Status = EChannelPackerStatus::Cancelled;
        Result.ErrorCode = TEXT("OperationCancelled");
        Result.Message = GetLocalizedMessage(
            TEXT("OperationCancelled"),
            TEXT("Texture generation was cancelled by user."),
            TEXT("テクスチャ生成がユーザーによってキャンセルされました。")
        ).ToString();
        Result.OutputAssets.Reset();
        return Result;
    };

    // STEP 1: Lock the inputs' source mips read-only (Game Thread). No full-resolution copy is
    // made: the workers read straight from the locked mips. Read locks are recursive, so the
    // same texture assigned to several slots is fine. Any early return below releases the
    // locks through the destructors.
    SlowTask.EnterProgressFrame(1.0f, GetLocalizedMessage(
        TEXT("ProgressExtracting"),
        TEXT("Extracting source data..."),
        TEXT("ソースデータを抽出中...")
    ));
    if (SlowTask.ShouldCancel())
    {
        return MakeCancelledResult();
    }

    FLockedTextureSource RawInputs[4]; // R, G, B, A
    {
        FPhaseTimer ExtractTimer(TEXT("Pack extract"));
        for (int32 Index = 0; Index < 4; ++Index)
        {
            RawInputs[Index] = FLockedTextureSource::Lock(InputTextures[Index]);
        }
    }

    // STEP 2: Extract and resize the four channels in parallel (background threads).
    SlowTask.EnterProgressFrame(2.0f, GetLocalizedMessage(
        TEXT("ProgressProcessingParallel"),
        TEXT("Resizing and processing channels..."),
        TEXT("チャンネルのリサイズと処理中...")
    ));
    if (SlowTask.ShouldCancel())
    {
        return MakeCancelledResult();
    }

    TArray<FTextureProcessResult> ProcessedResults;
    ProcessedResults.SetNum(4);
    const ESourceChannel SourceChannels[4] = { Request.Red.SourceChannel, Request.Green.SourceChannel, Request.Blue.SourceChannel, Request.Alpha.SourceChannel };
    {
        FPhaseTimer ProcessTimer(TEXT("Pack process"));
        ParallelFor(4, [&](int32 Index)
        {
            ProcessedResults[Index] = ProcessTextureSourceData(RawInputs[Index], Width, Height, SourceChannels[Index]);
        });
    }

    if (SlowTask.ShouldCancel())
    {
        return MakeCancelledResult();
    }

    // Report unreadable inputs (the slot is filled with its default, as in the editor tool),
    // then release the source mips: everything needed from here on lives in ProcessedResults.
    for (int32 Index = 0; Index < 4; ++Index)
    {
        const FTextureProcessResult& Processed = ProcessedResults[Index];
        FLockedTextureSource& Input = RawInputs[Index];

        if (!Processed.bSuccess && !Processed.ErrorMessage.IsEmpty())
        {
            AddWarning(Result, Processed.ErrorCode, GChannelLetters[Index], Processed.ErrorMessage);
        }
        else if (InputTextures[Index] && !Input.bIsValid)
        {
            if (!Input.ErrorMessage.IsEmpty())
            {
                AddWarning(Result, Input.ErrorCode, GChannelLetters[Index], Input.ErrorMessage);
            }
            else
            {
                AddWarning(Result, TEXT("ErrorInvalidSourceData"), GChannelLetters[Index], FText::Format(
                    GetLocalizedMessage(TEXT("ErrorInvalidSourceData"),
                        TEXT("{0} has no readable source data; the channel was filled with its default value."),
                        TEXT("{0} に読み取り可能なソースデータがないため、チャンネルをデフォルト値で埋めました。")),
                    FText::FromString(InputTextures[Index]->GetName())));
            }
        }
        Input.Release();
    }

    // STEP 3: Write the output texture (Game Thread).
    SlowTask.EnterProgressFrame(1.0f, GetLocalizedMessage(
        TEXT("ProgressWritingPixels"),
        TEXT("Writing pixel data..."),
        TEXT("ピクセルデータを書き込み中...")
    ));
    if (SlowTask.ShouldCancel())
    {
        return MakeCancelledResult();
    }

    FString PackageErrorCode;
    FText PackageErrorMessage;
    TStrongObjectPtr<UPackage> PackagePtr(PrepareOutputPackage(PackageName, AssetName, PackageErrorCode, PackageErrorMessage));
    UPackage* Package = PackagePtr.Get();
    if (!Package)
    {
        return SetFailed(Result, *PackageErrorCode, PackageErrorMessage);
    }

    UTexture2D* NewTexture = NewObject<UTexture2D>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_MarkAsRootSet);
    NewTexture->Source.Init(Width, Height, 1, 1, TSF_BGRA8);

    {
        FPhaseTimer WriteTimer(TEXT("Pack write"));
        uint8* MipData = NewTexture->Source.LockMip(0);
        if (MipData)
        {
            const bool bInvert[4] = { Request.Red.bInvert, Request.Green.bInvert, Request.Blue.bInvert, Request.Alpha.bInvert };

            // Default value for an empty/unreadable slot: R/G/B black (0), Alpha opaque (255).
            const uint8 DefaultValues[4] = { 0, 0, 0, 255 };

            // Resolve each slot to a W*H buffer up front (defaults pre-filled, invert applied) so
            // the interleaving loop below is branch-free.
            TArray<uint8> DefaultBuffers[4];
            const uint8* ChannelPtrs[4] = { nullptr, nullptr, nullptr, nullptr };
            for (int32 Index = 0; Index < 4; ++Index)
            {
                TArray<uint8>& Data = ProcessedResults[Index].ProcessedData.Num() > 0
                    ? ProcessedResults[Index].ProcessedData
                    : DefaultBuffers[Index];
                if (Data.Num() == 0)
                {
                    Data.Init(DefaultValues[Index], Width * Height);
                }
                if (bInvert[Index])
                {
                    for (uint8& Value : Data)
                    {
                        Value = 255 - Value;
                    }
                }
                ChannelPtrs[Index] = Data.GetData();
            }

            const uint8* PtrR = ChannelPtrs[0];
            const uint8* PtrG = ChannelPtrs[1];
            const uint8* PtrB = ChannelPtrs[2];
            const uint8* PtrA = ChannelPtrs[3];

            // Parallel, branch-free pixel writing
            ParallelFor(Width * Height, [MipData, PtrR, PtrG, PtrB, PtrA](int32 i)
            {
                int32 Offset = i * 4;
                MipData[Offset + 0] = PtrB[i]; // B
                MipData[Offset + 1] = PtrG[i]; // G
                MipData[Offset + 2] = PtrR[i]; // R
                MipData[Offset + 3] = PtrA[i]; // A
            });
        }
        NewTexture->Source.UnlockMip(0);
    }

    // STEP 4: Finalize (and save, if requested).
    SlowTask.EnterProgressFrame(1.0f, GetLocalizedMessage(
        TEXT("ProgressFinalizing"),
        TEXT("Finalizing texture..."),
        TEXT("テクスチャを最終処理中...")
    ));

    NewTexture->CompressionSettings = CompressionSetting;

    // Even if TC_Default is selected, treat it as linear (sRGB=false) for channel packing purposes.
    NewTexture->SRGB = false;

    {
        FPhaseTimer FinalizeTimer(TEXT("Pack finalize"));
        NewTexture->UpdateResource();
        NewTexture->PostEditChange();
    }

    Package->MarkPackageDirty();
    FAssetRegistryModule::AssetCreated(NewTexture);

    if (Request.bSave && !SaveAsset(Package, NewTexture))
    {
        return SetFailed(Result, TEXT("ErrorSaveFailed"), MakeSaveFailedMessage(PackageName));
    }

    Result.Status = EChannelPackerStatus::Succeeded;
    Result.Message = FText::Format(
        Request.bSave
            ? GetLocalizedMessage(TEXT("SuccessTextureSavedToDisk"), TEXT("Texture saved: {0}"), TEXT("テクスチャを保存しました: {0}"))
            : GetLocalizedMessage(TEXT("SuccessTextureCreated"), TEXT("Texture created: {0}"), TEXT("テクスチャを作成しました: {0}")),
        FText::FromString(PackageName)).ToString();
    return Result;
#else
    return SetFailed(Result, TEXT("ErrorNoEditorData"), GetLocalizedMessage(
        TEXT("ErrorNoEditorData"),
        TEXT("This plugin requires Editor-only data to function. Ensure the project is built with editor support."),
        TEXT("このプラグインはエディター専用データが必要です。プロジェクトがエディターサポート付きでビルドされていることを確認してください。")));
#endif
}

// ========== Unpack ==========

FChannelPackerResult Unpack(const FChannelPackerUnpackRequest& Request, const FExecutionOptions& Options)
{
    check(IsInGameThread());

    FPhaseTimer TotalTimer(TEXT("Unpack total"));
    FChannelPackerResult Result;

#if WITH_EDITORONLY_DATA
    // ---------------------------------------------------------
    // Validate and resolve the request
    // ---------------------------------------------------------
    if (Request.Source.IsNull())
    {
        return SetFailed(Result, TEXT("ErrorNoUnpackSource"), GetLocalizedMessage(
            TEXT("ErrorNoUnpackSource"),
            TEXT("Please select a source texture to unpack."),
            TEXT("アンパックするソーステクスチャを選択してください。")));
    }

    UTexture2D* SourceTex = ResolveTexture(Request.Source);
    if (!SourceTex)
    {
        return SetFailed(Result, TEXT("ErrorInputNotFound"), FText::Format(
            GetLocalizedMessage(TEXT("ErrorInputNotFound"),
                TEXT("Input texture not found (or not a Texture2D): {0}"),
                TEXT("入力テクスチャが見つからないか、Texture2D ではありません: {0}")),
            FText::FromString(Request.Source.ToString())));
    }

    const bool bRequested[4] = { Request.bExportRed, Request.bExportGreen, Request.bExportBlue, Request.bExportAlpha };
    if (!bRequested[0] && !bRequested[1] && !bRequested[2] && !bRequested[3])
    {
        return SetFailed(Result, TEXT("ErrorNoChannelsSelected"), GetLocalizedMessage(
            TEXT("ErrorNoChannelsSelected"),
            TEXT("Please select at least one channel to export."),
            TEXT("出力するチャンネルを少なくとも1つ選択してください。")));
    }

    const int32 SrcWidth = SourceTex->Source.GetSizeX();
    const int32 SrcHeight = SourceTex->Source.GetSizeY();
    if (SrcWidth < 1 || SrcHeight < 1)
    {
        return SetFailed(Result, TEXT("ErrorInvalidUnpackSource"), GetLocalizedMessage(
            TEXT("ErrorInvalidUnpackSource"),
            TEXT("The source texture has no valid source data."),
            TEXT("ソーステクスチャに有効なソースデータがありません。")));
    }
    const int64 NumSourcePixels = (int64)SrcWidth * (int64)SrcHeight;
    if (NumSourcePixels > (int64)MAX_int32)
    {
        return SetFailed(Result, TEXT("ErrorTextureTooLarge"), GetLocalizedMessage(
            TEXT("ErrorTextureTooLarge"),
            TEXT("Input texture is too large to process. Reduce its resolution."),
            TEXT("入力テクスチャが大きすぎて処理できません。解像度を下げてください。")));
    }
    Result.Width = SrcWidth;
    Result.Height = SrcHeight;

    // Output names: <Base><Suffix>, falling back to _R/_G/_B/_A for empty suffixes, which would
    // otherwise make several channels collide on the same asset name.
    static const TCHAR* const DefaultSuffixes[4] = { TEXT("_R"), TEXT("_G"), TEXT("_B"), TEXT("_A") };
    const FString* RequestedSuffixes[4] = { &Request.SuffixRed, &Request.SuffixGreen, &Request.SuffixBlue, &Request.SuffixAlpha };

    FString BaseName = Request.BaseName.TrimStartAndEnd();
    if (BaseName.IsEmpty())
    {
        BaseName = MakeUnpackBaseName(SourceTex->GetName(), GetKnownPackedSuffixes());
    }

    FString AssetNames[4];
    FString PackageNames[4];
    TSet<FString> UniqueNames;
    for (int32 Index = 0; Index < 4; ++Index)
    {
        AssetNames[Index] = BaseName + (RequestedSuffixes[Index]->IsEmpty() ? FString(DefaultSuffixes[Index]) : *RequestedSuffixes[Index]);
        PackageNames[Index] = JoinPackageName(Request.OutputPath, AssetNames[Index]);
        if (!bRequested[Index])
        {
            continue;
        }

        if (!ValidateOutputName(PackageNames[Index], AssetNames[Index], Result))
        {
            return Result;
        }

        bool bAlreadyInSet = false;
        UniqueNames.Add(AssetNames[Index], &bAlreadyInSet);
        if (bAlreadyInSet)
        {
            return SetFailed(Result, TEXT("ErrorDuplicateOutputNames"), GetLocalizedMessage(
                TEXT("ErrorDuplicateOutputNames"),
                TEXT("Two or more channels resolve to the same output name. Check the preset's channel suffixes."),
                TEXT("複数のチャンネルが同じ出力名になっています。プリセットのチャンネルサフィックスを確認してください。")));
        }
    }

    // ---------------------------------------------------------
    // Read the source (one read-only lock covers detection and extraction)
    // ---------------------------------------------------------
    int32 NumRequested = 0;
    for (bool bChannelRequested : bRequested)
    {
        NumRequested += bChannelRequested ? 1 : 0;
    }

    // Progress: 1 extract + one frame per saved channel.
    FScopedSlowTask SlowTask((float)(1 + NumRequested), GetLocalizedMessage(
        TEXT("ProgressUnpacking"),
        TEXT("Unpacking Texture..."),
        TEXT("テクスチャをアンパック中...")
    ));
    if (Options.bShowProgressDialog)
    {
        SlowTask.MakeDialog(true); // true = cancellable
    }

    const FText CancelMsg = GetLocalizedMessage(
        TEXT("UnpackCancelled"),
        TEXT("Unpack was cancelled by user."),
        TEXT("アンパックがユーザーによってキャンセルされました。")
    );

    SlowTask.EnterProgressFrame(1.0f, GetLocalizedMessage(
        TEXT("ProgressProcessingChannels"),
        TEXT("Extracting channels..."),
        TEXT("チャンネルを抽出中...")
    ));

    const ETextureSourceFormat SourceFormat = SourceTex->Source.GetFormat();

    TArray<int32> SelectedChannels;
    TArray<TArray<uint8>> ChannelData;
    {
        // Read-only lock: unlocking does not re-hash the payload or touch the source GUID.
        const uint8* Locked = SourceTex->Source.LockMipReadOnly(0);
        if (!Locked)
        {
            return SetFailed(Result, TEXT("ErrorLockFailed"), GetLocalizedMessage(
                TEXT("ErrorLockFailed"),
                TEXT("Failed to access texture data. The texture may be corrupted or in use. Try reimporting the texture."),
                TEXT("テクスチャデータへのアクセスに失敗しました。テクスチャが破損しているか、使用中の可能性があります。テクスチャを再インポートしてください。")));
        }
        ON_SCOPE_EXIT
        {
            SourceTex->Source.UnlockMip(0);
        };

        // Uniform detection reads every pixel of each requested channel, so it only runs when asked for.
        bool bUniform[4] = { false, false, false, false };
        const bool bFormatSupported = VisitChannelSampler(Locked, SourceFormat, [&](auto&& Sample)
        {
            if (Request.bSkipUniformChannels)
            {
                FPhaseTimer DetectTimer(TEXT("Unpack detect uniform"));
                DetectUniformChannels(Sample, SrcWidth, SrcHeight, bRequested, bUniform);
            }
        });

        if (!bFormatSupported)
        {
            UE_LOG(LogTexturePacker, Error, TEXT("Unsupported Source Format: %d for texture: %s"), (int32)SourceFormat, *SourceTex->GetName());
            return SetFailed(Result, TEXT("ErrorUnsupportedFormat"), GetLocalizedMessage(
                TEXT("ErrorUnsupportedFormat"),
                TEXT("Texture format not supported. Please convert to PNG or TGA."),
                TEXT("テクスチャ形式がサポートされていません。PNGまたはTGAに変換してください。")));
        }

        for (int32 Index = 0; Index < 4; ++Index)
        {
            if (!bRequested[Index])
            {
                continue;
            }
            if (bUniform[Index])
            {
                Result.SkippedUniformChannels.Add(GChannelLetters[Index]);
                continue;
            }
            SelectedChannels.Add(Index);
            Result.OutputAssets.Add(PackageNames[Index] + TEXT(".") + AssetNames[Index]);
        }

        if (SelectedChannels.Num() == 0)
        {
            return SetFailed(Result, TEXT("ErrorNoChannelsToExport"), GetLocalizedMessage(
                TEXT("ErrorNoChannelsToExport"),
                TEXT("Every requested channel holds a single value, so there is nothing to export. Disable uniform-channel skipping to export them anyway."),
                TEXT("要求されたチャンネルはすべて単一の値のみのため、出力するものがありません。それでも出力する場合は均一チャンネルのスキップを無効にしてください。")));
        }

        if (Request.OverwritePolicy != EChannelPackerOverwritePolicy::Overwrite)
        {
            TArray<FString> ExistingNames;
            for (int32 ChannelIndex : SelectedChannels)
            {
                if (DoesAssetExist(PackageNames[ChannelIndex], AssetNames[ChannelIndex]))
                {
                    ExistingNames.Add(PackageNames[ChannelIndex]);
                }
            }
            if (ExistingNames.Num() > 0)
            {
                const FText ExistingList = FText::FromString(FString::Join(ExistingNames, TEXT(", ")));
                if (Request.OverwritePolicy == EChannelPackerOverwritePolicy::Skip)
                {
                    Result.Status = EChannelPackerStatus::Skipped;
                    Result.Message = FText::Format(
                        GetLocalizedMessage(TEXT("SkippedAssetExists"),
                            TEXT("{0} already exists; skipped."),
                            TEXT("{0} は既に存在するためスキップしました。")),
                        ExistingList).ToString();
                    return Result;
                }
                return SetFailed(Result, TEXT("ErrorAssetExists"), FText::Format(
                    GetLocalizedMessage(TEXT("ErrorAssetExists"),
                        TEXT("{0} already exists. Use the Overwrite or Skip overwrite policy."),
                        TEXT("{0} は既に存在します。上書きポリシーに Overwrite または Skip を指定してください。")),
                    ExistingList));
            }
        }

        if (Request.bDryRun)
        {
            Result.Status = EChannelPackerStatus::DryRun;
            Result.Message = FText::Format(
                GetLocalizedMessage(TEXT("DryRunUnpack"),
                    TEXT("Dry run: would create {0} texture(s) in {1}."),
                    TEXT("ドライラン: {1} に {0} 枚のテクスチャを作成します。")),
                FText::AsNumber(SelectedChannels.Num()), FText::FromString(Request.OutputPath)).ToString();
            return Result;
        }

        if (SlowTask.ShouldCancel())
        {
            Result.OutputAssets.Reset();
            Result.Status = EChannelPackerStatus::Cancelled;
            Result.ErrorCode = TEXT("UnpackCancelled");
            Result.Message = CancelMsg.ToString();
            return Result;
        }

        // Output resolution always matches the source, so no resize (and therefore no FColor
        // conversion) is needed: each channel is read straight out of the locked mip, and only
        // the one-byte-per-pixel output buffers are allocated.
        FPhaseTimer ExtractTimer(TEXT("Unpack extract"));
        ChannelData.SetNum(SelectedChannels.Num());
        VisitChannelSampler(Locked, SourceFormat, [&](auto&& Sample)
        {
            // One channel at a time; each extraction parallelizes internally over pixels.
            for (int32 Index = 0; Index < SelectedChannels.Num(); ++Index)
            {
                ExtractChannelBytes(Sample, SelectedChannels[Index], NumSourcePixels, ChannelData[Index]);
            }
        });
    }

    // ---------------------------------------------------------
    // Write one grayscale asset per channel (Game Thread)
    // ---------------------------------------------------------
    Result.OutputAssets.Reset();
    bool bCancelled = false;
    TArray<FString> SaveFailures;

    for (int32 SelectionIndex = 0; SelectionIndex < SelectedChannels.Num(); ++SelectionIndex)
    {
        const int32 ChannelIndex = SelectedChannels[SelectionIndex];
        const FString& AssetName = AssetNames[ChannelIndex];
        const FString& PackageName = PackageNames[ChannelIndex];

        SlowTask.EnterProgressFrame(1.0f, FText::Format(
            GetLocalizedMessage(TEXT("ProgressSavingChannel"), TEXT("Saving {0}..."), TEXT("{0} を保存中...")),
            FText::FromString(AssetName)
        ));

        if (SlowTask.ShouldCancel())
        {
            bCancelled = true;
            break;
        }

        const TArray<uint8>& Data = ChannelData[SelectionIndex];
        if ((int64)Data.Num() != NumSourcePixels)
        {
            UE_LOG(LogTexturePacker, Error, TEXT("Unexpected channel data size for %s (%d, expected %lld). Skipping."),
                *AssetName, Data.Num(), NumSourcePixels);
            continue;
        }

        FString PackageErrorCode;
        FText PackageErrorMessage;
        TStrongObjectPtr<UPackage> PackagePtr(PrepareOutputPackage(PackageName, AssetName, PackageErrorCode, PackageErrorMessage));
        UPackage* Package = PackagePtr.Get();
        if (!Package)
        {
            AddWarning(Result, PackageErrorCode, GChannelLetters[ChannelIndex], PackageErrorMessage);
            continue;
        }

        UTexture2D* NewTexture = NewObject<UTexture2D>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_MarkAsRootSet);

        NewTexture->Source.Init(SrcWidth, SrcHeight, 1, 1, TSF_G8);
        uint8* MipData = NewTexture->Source.LockMip(0);
        if (MipData)
        {
            FMemory::Memcpy(MipData, Data.GetData(), Data.Num());
        }
        NewTexture->Source.UnlockMip(0);

        // Extracted channels are data, not color: grayscale compression, linear color space.
        NewTexture->CompressionSettings = TC_Grayscale;
        NewTexture->SRGB = false;

        NewTexture->UpdateResource();
        NewTexture->PostEditChange();

        Package->MarkPackageDirty();
        FAssetRegistryModule::AssetCreated(NewTexture);
        Result.OutputAssets.Add(PackageName + TEXT(".") + AssetName);

        if (Request.bSave && !SaveAsset(Package, NewTexture))
        {
            SaveFailures.Add(PackageName);
            AddWarning(Result, TEXT("ErrorSaveFailed"), GChannelLetters[ChannelIndex], MakeSaveFailedMessage(PackageName));
        }
    }

    if (bCancelled)
    {
        Result.Status = EChannelPackerStatus::Cancelled;
        Result.ErrorCode = TEXT("UnpackCancelled");
        Result.Message = CancelMsg.ToString();
        return Result;
    }
    if (SaveFailures.Num() > 0)
    {
        return SetFailed(Result, TEXT("ErrorSaveFailed"), MakeSaveFailedMessage(FString::Join(SaveFailures, TEXT(", "))));
    }
    if (Result.OutputAssets.Num() == 0)
    {
        return SetFailed(Result, TEXT("ErrorUnpackNothingSaved"), GetLocalizedMessage(
            TEXT("ErrorUnpackNothingSaved"),
            TEXT("No textures could be saved."),
            TEXT("テクスチャを保存できませんでした。")));
    }

    Result.Status = EChannelPackerStatus::Succeeded;
    Result.Message = FText::Format(
        GetLocalizedMessage(
            TEXT("SuccessUnpacked"),
            TEXT("Unpacked {0} texture(s) to {1}"),
            TEXT("{0} 枚のテクスチャを {1} にアンパックしました")
        ),
        FText::AsNumber(Result.OutputAssets.Num()),
        FText::FromString(Request.OutputPath)).ToString();
    return Result;
#else
    return SetFailed(Result, TEXT("ErrorNoEditorData"), GetLocalizedMessage(
        TEXT("ErrorNoEditorData"),
        TEXT("This plugin requires Editor-only data to function. Ensure the project is built with editor support."),
        TEXT("このプラグインはエディター専用データが必要です。プロジェクトがエディターサポート付きでビルドされていることを確認してください。")));
#endif
}

// ========== Naming ==========

FString MakePackedAssetName(const TArray<FString>& InputNames, const FString& Suffix)
{
    if (InputNames.Num() == 0)
    {
        return FString();
    }

    // Find Common Prefix
    FString CommonPrefix = InputNames[0];
    for (int32 i = 1; i < InputNames.Num(); ++i)
    {
        const FString& CurrentName = InputNames[i];
        int32 CommonLen = 0;
        int32 MaxLen = FMath::Min(CommonPrefix.Len(), CurrentName.Len());
        for (int32 CharIdx = 0; CharIdx < MaxLen; ++CharIdx)
        {
            if (CommonPrefix[CharIdx] == CurrentName[CharIdx])
            {
                CommonLen++;
            }
            else
            {
                break;
            }
        }
        CommonPrefix = CommonPrefix.Left(CommonLen);
    }

    FString BaseName;
    if (CommonPrefix.Len() >= 3)
    {
        BaseName = CommonPrefix;
    }
    else
    {
        BaseName = InputNames[0]; // First valid input
    }

    // Enforce "T_" prefix
    if (!BaseName.StartsWith(TEXT("T_")))
    {
        BaseName = TEXT("T_") + BaseName;
    }

    // Remove trailing underscores
    while (BaseName.EndsWith(TEXT("_")))
    {
        BaseName.LeftChopInline(1);
    }

    return BaseName + Suffix;
}

FString MakeUnpackBaseName(const FString& SourceName, const TArray<FString>& KnownPackedSuffixes)
{
    FString BaseName = SourceName;

    // Strip a known packed suffix (any preset's filename suffix, e.g. "_ORM", "_MRA",
    // "_Packed"), longest first so "_Packed" wins over a hypothetical "_P".
    TArray<FString> Suffixes = KnownPackedSuffixes;
    Suffixes.Sort([](const FString& A, const FString& B) { return A.Len() > B.Len(); });

    for (const FString& Suffix : Suffixes)
    {
        if (!Suffix.IsEmpty() && BaseName.Len() > Suffix.Len() && BaseName.EndsWith(Suffix, ESearchCase::IgnoreCase))
        {
            BaseName.LeftChopInline(Suffix.Len());
            break;
        }
    }

    // Enforce "T_" prefix
    if (!BaseName.StartsWith(TEXT("T_")))
    {
        BaseName = TEXT("T_") + BaseName;
    }

    // Remove trailing underscores
    while (BaseName.EndsWith(TEXT("_")))
    {
        BaseName.LeftChopInline(1);
    }

    return BaseName;
}

// ========== Compression ==========

TArray<FCompressionOption> GetCompressionOptions()
{
    TArray<FCompressionOption> Options;

    FCompressionOption& MasksOption = Options.AddDefaulted_GetRef();
    MasksOption.InternalName = TEXT("Masks");
    MasksOption.CompressionSetting = TC_Masks;
    MasksOption.DisplayNameEn = TEXT("Masks (Recommended)");
    MasksOption.DisplayNameJa = TEXT("マスク (推奨)");

    FCompressionOption& GrayscaleOption = Options.AddDefaulted_GetRef();
    GrayscaleOption.InternalName = TEXT("Grayscale");
    GrayscaleOption.CompressionSetting = TC_Grayscale;
    GrayscaleOption.DisplayNameEn = TEXT("Grayscale");
    GrayscaleOption.DisplayNameJa = TEXT("グレースケール");

    FCompressionOption& DefaultOption = Options.AddDefaulted_GetRef();
    DefaultOption.InternalName = TEXT("Default");
    DefaultOption.CompressionSetting = TC_Default;
    DefaultOption.DisplayNameEn = TEXT("Default");
    DefaultOption.DisplayNameJa = TEXT("デフォルト");

    return Options;
}

bool FindCompressionSetting(const FString& Name, TextureCompressionSettings& OutSetting)
{
    for (const FCompressionOption& Option : GetCompressionOptions())
    {
        if (Option.InternalName.Equals(Name.TrimStartAndEnd(), ESearchCase::IgnoreCase))
        {
            OutSetting = Option.CompressionSetting;
            return true;
        }
    }
    return false;
}

// ========== Presets ==========

TArray<FChannelPackerPreset> GetBuiltInPresets()
{
    TArray<FChannelPackerPreset> Result;

    // Custom (sentinel - always first)
    {
        FChannelPackerPreset& Preset = Result.AddDefaulted_GetRef();
        Preset.PresetName = TEXT("Custom");
        Preset.bIsBuiltIn = true;
        Preset.RedLabelEn = TEXT("Red Channel Input");
        Preset.RedLabelJa = TEXT("Red Channel Input");
        Preset.GreenLabelEn = TEXT("Green Channel Input");
        Preset.GreenLabelJa = TEXT("Green Channel Input");
        Preset.BlueLabelEn = TEXT("Blue Channel Input");
        Preset.BlueLabelJa = TEXT("Blue Channel Input");
        Preset.AlphaLabelEn = TEXT("Alpha Channel Input (Optional)");
        Preset.AlphaLabelJa = TEXT("Alpha Channel Input (任意)");
        Preset.FileNameSuffix = TEXT("_Packed");
        Preset.DefaultCompressionName = TEXT("Masks");
        // Unpack suffixes keep the plain _R/_G/_B/_A defaults for Custom.
    }

    // ORM (default)
    {
        FChannelPackerPreset& Preset = Result.AddDefaulted_GetRef();
        Preset.PresetName = TEXT("ORM");
        Preset.bIsBuiltIn = true;
        Preset.RedLabelEn = TEXT("Red Channel Input (e.g. Ambient Occlusion)");
        Preset.RedLabelJa = TEXT("Red Channel Input (例: アンビエントオクルージョン)");
        Preset.GreenLabelEn = TEXT("Green Channel Input (e.g. Roughness)");
        Preset.GreenLabelJa = TEXT("Green Channel Input (例: ラフネス)");
        Preset.BlueLabelEn = TEXT("Blue Channel Input (e.g. Metallic)");
        Preset.BlueLabelJa = TEXT("Blue Channel Input (例: メタリック)");
        Preset.AlphaLabelEn = TEXT("Alpha Channel Input (Optional)");
        Preset.AlphaLabelJa = TEXT("Alpha Channel Input (任意)");
        Preset.FileNameSuffix = TEXT("_ORM");
        Preset.DefaultCompressionName = TEXT("Masks");
        Preset.UnpackSuffixR = TEXT("_AO");
        Preset.UnpackSuffixG = TEXT("_Roughness");
        Preset.UnpackSuffixB = TEXT("_Metallic");
        Preset.UnpackSuffixA = TEXT("_A");
    }

    // MRA
    {
        FChannelPackerPreset& Preset = Result.AddDefaulted_GetRef();
        Preset.PresetName = TEXT("MRA");
        Preset.bIsBuiltIn = true;
        Preset.RedLabelEn = TEXT("Red Channel Input (e.g. Metallic)");
        Preset.RedLabelJa = TEXT("Red Channel Input (例: メタリック)");
        Preset.GreenLabelEn = TEXT("Green Channel Input (e.g. Roughness)");
        Preset.GreenLabelJa = TEXT("Green Channel Input (例: ラフネス)");
        Preset.BlueLabelEn = TEXT("Blue Channel Input (e.g. Ambient Occlusion)");
        Preset.BlueLabelJa = TEXT("Blue Channel Input (例: アンビエントオクルージョン)");
        Preset.AlphaLabelEn = TEXT("Alpha Channel Input (Optional)");
        Preset.AlphaLabelJa = TEXT("Alpha Channel Input (任意)");
        Preset.FileNameSuffix = TEXT("_MRA");
        Preset.DefaultCompressionName = TEXT("Masks");
        Preset.UnpackSuffixR = TEXT("_Metallic");
        Preset.UnpackSuffixG = TEXT("_Roughness");
        Preset.UnpackSuffixB = TEXT("_AO");
        Preset.UnpackSuffixA = TEXT("_A");
    }

    return Result;
}

TArray<FChannelPackerPreset> LoadUserPresets()
{
    TArray<FChannelPackerPreset> Result;
    FString PresetsDir = GetPresetsDirectory();

    TArray<FString> FoundFiles;
    IFileManager::Get().FindFiles(FoundFiles, *(PresetsDir / TEXT("*.json")), true, false);

    for (const FString& FileName : FoundFiles)
    {
        FString FilePath = PresetsDir / FileName;
        FString JsonString;
        if (FFileHelper::LoadFileToString(JsonString, *FilePath))
        {
            TSharedPtr<FJsonObject> JsonObject;
            TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
            if (FJsonSerializer::Deserialize(Reader, JsonObject) && JsonObject.IsValid())
            {
                FChannelPackerPreset Preset = FChannelPackerPreset::FromJson(JsonObject);
                if (!Preset.PresetName.IsEmpty())
                {
                    Result.Add(Preset);
                    UE_LOG(LogTexturePacker, Log, TEXT("Loaded user preset: %s"), *Preset.PresetName);
                }
            }
            else
            {
                UE_LOG(LogTexturePacker, Warning, TEXT("Failed to parse preset file: %s"), *FilePath);
            }
        }
    }
    return Result;
}

bool SaveUserPreset(const FChannelPackerPreset& Preset)
{
    FString PresetsDir = GetPresetsDirectory();
    IFileManager::Get().MakeDirectory(*PresetsDir, true);

    FString FileName = SanitizePresetFileName(Preset.PresetName) + TEXT(".json");
    FString FilePath = PresetsDir / FileName;

    TSharedPtr<FJsonObject> JsonObject = Preset.ToJson();
    FString JsonString;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
    if (FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer))
    {
        return FFileHelper::SaveStringToFile(JsonString, *FilePath);
    }
    return false;
}

bool DeleteUserPreset(const FString& PresetName)
{
    FString PresetsDir = GetPresetsDirectory();
    FString FileName = SanitizePresetFileName(PresetName) + TEXT(".json");
    FString FilePath = PresetsDir / FileName;
    return IFileManager::Get().Delete(*FilePath);
}

TArray<FChannelPackerPreset> GetAllPresets()
{
    TArray<FChannelPackerPreset> Result = GetBuiltInPresets();
    Result.Append(LoadUserPresets());
    return Result;
}

bool FindPreset(const FString& PresetName, FChannelPackerPreset& OutPreset)
{
    const FString Name = PresetName.TrimStartAndEnd();
    for (const FChannelPackerPreset& Preset : GetAllPresets())
    {
        if (Preset.PresetName.Equals(Name, ESearchCase::IgnoreCase))
        {
            OutPreset = Preset;
            return true;
        }
    }
    return false;
}

TArray<FString> GetKnownPackedSuffixes()
{
    TArray<FString> Suffixes;
    for (const FChannelPackerPreset& Preset : GetAllPresets())
    {
        if (!Preset.FileNameSuffix.IsEmpty())
        {
            Suffixes.AddUnique(Preset.FileNameSuffix);
        }
    }
    Suffixes.Sort([](const FString& A, const FString& B) { return A.Len() > B.Len(); });
    return Suffixes;
}

void ApplyPresetToPackRequest(const FChannelPackerPreset& Preset, FChannelPackerPackRequest& Request)
{
    Request.Red.SourceChannel = Preset.DefaultSourceChannelR;
    Request.Green.SourceChannel = Preset.DefaultSourceChannelG;
    Request.Blue.SourceChannel = Preset.DefaultSourceChannelB;
    Request.Alpha.SourceChannel = Preset.DefaultSourceChannelA;

    Request.Red.bInvert = Preset.bDefaultInvertR;
    Request.Green.bInvert = Preset.bDefaultInvertG;
    Request.Blue.bInvert = Preset.bDefaultInvertB;
    Request.Alpha.bInvert = Preset.bDefaultInvertA;

    Request.Compression = Preset.DefaultCompressionName.IsEmpty() ? FString(TEXT("Masks")) : Preset.DefaultCompressionName;
    Request.FileNameSuffix = Preset.FileNameSuffix;
}

void ApplyPresetToUnpackRequest(const FChannelPackerPreset& Preset, FChannelPackerUnpackRequest& Request)
{
    Request.SuffixRed = Preset.UnpackSuffixR;
    Request.SuffixGreen = Preset.UnpackSuffixG;
    Request.SuffixBlue = Preset.UnpackSuffixB;
    Request.SuffixAlpha = Preset.UnpackSuffixA;
}

// ========== Misc ==========

FString StatusToString(EChannelPackerStatus Status)
{
    switch (Status)
    {
    case EChannelPackerStatus::Succeeded: return TEXT("Succeeded");
    case EChannelPackerStatus::Skipped:   return TEXT("Skipped");
    case EChannelPackerStatus::DryRun:    return TEXT("DryRun");
    case EChannelPackerStatus::Failed:    return TEXT("Failed");
    case EChannelPackerStatus::Cancelled: return TEXT("Cancelled");
    }
    return TEXT("Failed");
}

} // namespace TextureChannelPackerCore
