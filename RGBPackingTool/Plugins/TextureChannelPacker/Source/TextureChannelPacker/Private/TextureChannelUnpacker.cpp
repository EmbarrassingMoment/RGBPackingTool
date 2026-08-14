#include "TextureChannelUnpacker.h"
#include "TextureChannelPackerShared.h"
#include "Engine/Texture2D.h"
// FTexturePlatformData lives in a dedicated header on newer engine versions but is
// declared inside Engine/Texture.h (pulled in via Engine/Texture2D.h above) on older
// ones. Guard the include so the build succeeds regardless of where it resides.
#if __has_include("Engine/TexturePlatformData.h")
#include "Engine/TexturePlatformData.h"
#endif
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/Paths.h"
#include "Misc/MessageDialog.h"
#include "Misc/ScopedSlowTask.h"
#include "Math/UnrealMathUtility.h"
#include "Math/Float16.h"
#include "Async/ParallelFor.h"
#include "PropertyCustomizationHelpers.h"
#include "ThumbnailRendering/ThumbnailManager.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "Styling/AppStyle.h"
#include "Styling/SlateBrush.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/Layout/SUniformGridPanel.h"

#define LOCTEXT_NAMESPACE "FTextureChannelUnpacker"

using namespace TextureChannelPackerUtils;

/** Channel processing order for the unpack grid (R=0, G=1, B=2, A=3). */
static const ESourceChannel GUnpackChannelOrder[4] = { ESourceChannel::Red, ESourceChannel::Green, ESourceChannel::Blue, ESourceChannel::Alpha };
static const TCHAR* GUnpackChannelLetters[4] = { TEXT("R"), TEXT("G"), TEXT("B"), TEXT("A") };

/**
 * @brief Detects whether every pixel of the requested channel holds the same value.
 *
 * Runs on the full-resolution raw data so detection is exact (a downscaled preview could
 * average away small variations). Cheap in practice: non-uniform channels early-exit on
 * the first differing pixel. Values are compared after the same 8-bit conversion the
 * processing pipeline applies, so the reported value matches the preview and the output.
 *
 * @param Input Raw source data (must be valid).
 * @param Channel Which channel to inspect.
 * @param OutValue Receives the uniform value when the function returns true.
 * @return True if the channel is uniform (likely unused).
 */
static bool ComputeChannelUniformValue(const FTextureRawData& Input, ESourceChannel Channel, uint8& OutValue)
{
    OutValue = 0;
    if (!Input.bIsValid || Input.Width <= 0 || Input.Height <= 0)
    {
        return false;
    }

    // Single-channel sources carry no alpha; the packing pipeline treats it as opaque.
    if (IsSingleChannelFormat(Input.Format) && Channel == ESourceChannel::Alpha)
    {
        OutValue = 255;
        return true;
    }

    const int64 NumPixels = (int64)Input.Width * (int64)Input.Height;
    const uint8* Src = Input.RawData.GetData();

    // Scans converted 8-bit values, early-exiting on the first mismatch.
    auto ScanConverted = [NumPixels, &OutValue](auto GetValueAt) -> bool
    {
        const uint8 First = GetValueAt((int64)0);
        for (int64 i = 1; i < NumPixels; ++i)
        {
            if (GetValueAt(i) != First)
            {
                return false;
            }
        }
        OutValue = First;
        return true;
    };

    switch (Input.Format)
    {
    case TSF_BGRA8:
    {
        const int32 Offset = GetBGRAChannelOffset(Channel);
        return ScanConverted([Src, Offset](int64 i) { return Src[i * 4 + Offset]; });
    }
    case TSF_G8:
    {
        return ScanConverted([Src](int64 i) { return Src[i]; });
    }
    case TSF_G16:
    {
        const uint16* Pixels = (const uint16*)Src;
        return ScanConverted([Pixels](int64 i) { return (uint8)(Pixels[i] >> 8); });
    }
    case TSF_R16F:
    {
        const FFloat16* Pixels = (const FFloat16*)Src;
        return ScanConverted([Pixels](int64 i) { return (uint8)FMath::Clamp<float>((float)Pixels[i] * 255.0f, 0.0f, 255.0f); });
    }
    case TSF_R32F:
    {
        const float* Pixels = (const float*)Src;
        return ScanConverted([Pixels](int64 i) { return (uint8)FMath::Clamp<float>(Pixels[i] * 255.0f, 0.0f, 255.0f); });
    }
    case TSF_RGBA32F:
    {
        const FLinearColor* Pixels = (const FLinearColor*)Src;
        return ScanConverted([Pixels, Channel](int64 i)
        {
            const FLinearColor& LC = Pixels[i];
            float Value;
            switch (Channel)
            {
            case ESourceChannel::Red:   Value = LC.R; break;
            case ESourceChannel::Green: Value = LC.G; break;
            case ESourceChannel::Blue:  Value = LC.B; break;
            default:                    Value = LC.A; break;
            }
            return (uint8)FMath::Clamp<float>(Value * 255.0f, 0.0f, 255.0f);
        });
    }
    default:
        return false;
    }
}

void FTextureChannelUnpacker::Initialize(const TArray<TSharedPtr<FChannelPackerPreset>>* InPresets, TSharedPtr<FChannelPackerPreset> InDefaultPreset)
{
    Presets = InPresets;
    CurrentPreset = InDefaultPreset;
}

void FTextureChannelUnpacker::ReleaseResources()
{
    for (int32 Index = 0; Index < 4; ++Index)
    {
        if (ChannelBrushes[Index].IsValid())
        {
            ChannelBrushes[Index]->SetResourceObject(nullptr);
        }
        ChannelBrushes[Index].Reset();
        ChannelPreviewTextures[Index].Reset();
    }
}

void FTextureChannelUnpacker::OnPresetListChanged()
{
    if (Presets && CurrentPreset.IsValid() && !Presets->Contains(CurrentPreset))
    {
        // The selected preset was deleted; fall back to the first preset (Custom).
        CurrentPreset = Presets->Num() > 0 ? (*Presets)[0] : nullptr;
    }
    if (PresetComboBox.IsValid())
    {
        PresetComboBox->RefreshOptions();
        if (CurrentPreset.IsValid())
        {
            PresetComboBox->SetSelectedItem(CurrentPreset);
        }
    }
}

FString FTextureChannelUnpacker::GetChannelSuffix(int32 ChannelIndex) const
{
    static const TCHAR* DefaultSuffixes[4] = { TEXT("_R"), TEXT("_G"), TEXT("_B"), TEXT("_A") };

    if (CurrentPreset.IsValid())
    {
        FString Suffix;
        switch (ChannelIndex)
        {
        case 0: Suffix = CurrentPreset->UnpackSuffixR; break;
        case 1: Suffix = CurrentPreset->UnpackSuffixG; break;
        case 2: Suffix = CurrentPreset->UnpackSuffixB; break;
        case 3: Suffix = CurrentPreset->UnpackSuffixA; break;
        default: break;
        }
        // Guard against empty suffixes (e.g. hand-edited preset files), which would make
        // several channels collide on the same asset name.
        if (!Suffix.IsEmpty())
        {
            return Suffix;
        }
    }
    return DefaultSuffixes[FMath::Clamp(ChannelIndex, 0, 3)];
}

FString FTextureChannelUnpacker::GetChannelAssetName(int32 ChannelIndex) const
{
    return BaseFileName + GetChannelSuffix(ChannelIndex);
}

FString FTextureChannelUnpacker::GetChannelPackageName(int32 ChannelIndex) const
{
    FString PackageName = OutputPackagePath;
    if (!PackageName.EndsWith(TEXT("/")))
    {
        PackageName += TEXT("/");
    }
    return PackageName + GetChannelAssetName(ChannelIndex);
}

FText FTextureChannelUnpacker::GetChannelHeaderText(int32 ChannelIndex) const
{
    const TCHAR* Letter = GUnpackChannelLetters[FMath::Clamp(ChannelIndex, 0, 3)];

    // Derive a short meaning from the suffix (e.g. "_AO" -> "AO"). If it adds nothing
    // over the channel letter itself, show just the letter.
    FString Meaning = GetChannelSuffix(ChannelIndex);
    Meaning.RemoveFromStart(TEXT("_"));
    if (Meaning.IsEmpty() || Meaning.Equals(Letter, ESearchCase::IgnoreCase))
    {
        return FText::FromString(Letter);
    }
    return FText::FromString(FString::Printf(TEXT("%s → %s"), Letter, *Meaning));
}

void FTextureChannelUnpacker::ApplyPreset(TSharedPtr<FChannelPackerPreset> Preset)
{
    if (!Preset.IsValid())
    {
        return;
    }

    CurrentPreset = Preset;

    // Re-derive the base name so packed suffixes are stripped consistently.
    // Manual edits are preserved (AutoGenerateBaseName respects the manual-edit flag).
    AutoGenerateBaseName();

    if (PresetComboBox.IsValid())
    {
        PresetComboBox->SetSelectedItem(Preset);
    }
}

void FTextureChannelUnpacker::AutoGenerateBaseName()
{
    if (bBaseNameManuallyEdited || !SourceTexture.IsValid())
    {
        return;
    }

    FString BaseName = SourceTexture->GetName();

    // Strip a known packed suffix (any preset's filename suffix, e.g. "_ORM", "_MRA",
    // "_Packed"), longest first so "_Packed" wins over a hypothetical "_P".
    TArray<FString> KnownSuffixes;
    if (Presets)
    {
        for (const TSharedPtr<FChannelPackerPreset>& Preset : *Presets)
        {
            if (Preset.IsValid() && !Preset->FileNameSuffix.IsEmpty())
            {
                KnownSuffixes.AddUnique(Preset->FileNameSuffix);
            }
        }
    }
    KnownSuffixes.Sort([](const FString& A, const FString& B) { return A.Len() > B.Len(); });

    for (const FString& Suffix : KnownSuffixes)
    {
        if (BaseName.Len() > Suffix.Len() && BaseName.EndsWith(Suffix, ESearchCase::IgnoreCase))
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

    BaseFileName = BaseName;
}

void FTextureChannelUnpacker::OnSourceTextureChanged(UTexture2D* NewTexture)
{
    SourceTexture = NewTexture;
    AutoGenerateBaseName();
    UpdatePreview();
}

void FTextureChannelUnpacker::UpdatePreview()
{
    check(IsInGameThread());

    bPreviewValid = false;

    UTexture2D* SourceTex = SourceTexture.Get();
    if (!SourceTex)
    {
        // Source cleared: reset channel state to defaults.
        for (int32 Index = 0; Index < 4; ++Index)
        {
            bExportChannel[Index] = true;
            bChannelUniform[Index] = false;
            ChannelUniformValue[Index] = 0;
        }
        return;
    }

    // Extract source data on the Game Thread.
    FTextureRawData Raw = ExtractTextureSourceData(SourceTex);
    if (!Raw.bIsValid)
    {
        if (!Raw.ErrorMessage.IsEmpty())
        {
            ShowNotification(Raw.ErrorMessage, false);
        }
        return;
    }

    // Uniform-value detection on the full-resolution data. Channels that hold a single
    // value are likely unused, so they are excluded from export by default (the user can
    // re-check the box to export them anyway).
    for (int32 Index = 0; Index < 4; ++Index)
    {
        bChannelUniform[Index] = ComputeChannelUniformValue(Raw, GUnpackChannelOrder[Index], ChannelUniformValue[Index]);
        bExportChannel[Index] = !bChannelUniform[Index];
    }

    // Derive a small preview resolution from the source aspect ratio so the preview stays
    // cheap to build even for very large sources (e.g. 16K).
    const int32 PreviewMaxDim = 256;
    const float Scale = FMath::Min(1.0f, (float)PreviewMaxDim / (float)FMath::Max(Raw.Width, Raw.Height));
    const int32 W = FMath::Max(1, FMath::RoundToInt(Raw.Width * Scale));
    const int32 H = FMath::Max(1, FMath::RoundToInt(Raw.Height * Scale));
    const int32 NumPixels = W * H;

    // Process all four channels in parallel. The raw data is shared between the tasks,
    // so pass bCanConsumeInput=false to keep every task read-only.
    FTextureProcessResult Results[4];
    ParallelFor(4, [&Raw, &Results, W, H](int32 Index)
    {
        Results[Index] = ProcessTextureSourceData(Raw, W, H, GUnpackChannelOrder[Index], /*bCanConsumeInput=*/false);
    });

    // Build one grayscale transient texture per channel.
    for (int32 Index = 0; Index < 4; ++Index)
    {
        TArray<uint8>& Data = Results[Index].ProcessedData;
        if (Data.Num() != NumPixels)
        {
            // Fallback matching packing defaults: RGB to black, Alpha to opaque.
            Data.Init(Index == 3 ? 255 : 0, NumPixels);
        }

        // Replicate the channel value across B/G/R; the displayed alpha is forced opaque
        // so the preview is never blended against the panel background.
        TArray<uint8> BGRA;
        BGRA.SetNumUninitialized(NumPixels * 4);
        for (int32 i = 0; i < NumPixels; ++i)
        {
            const uint8 Value = Data[i];
            const int32 Offset = i * 4;
            BGRA[Offset + 0] = Value;
            BGRA[Offset + 1] = Value;
            BGRA[Offset + 2] = Value;
            BGRA[Offset + 3] = 255;
        }

        UTexture2D* NewPreview = UTexture2D::CreateTransient(W, H, PF_B8G8R8A8);
        if (!NewPreview)
        {
            UE_LOG(LogTexturePacker, Warning, TEXT("Failed to create transient unpack preview texture (channel %d)."), Index);
            continue;
        }

        // Show the literal channel values (linear), matching the extracted assets' color space.
        NewPreview->SRGB = false;
        NewPreview->Filter = TF_Nearest;

        if (FTexturePlatformData* PlatformData = NewPreview->GetPlatformData())
        {
            void* MipData = PlatformData->Mips[0].BulkData.Lock(LOCK_READ_WRITE);
            FMemory::Memcpy(MipData, BGRA.GetData(), BGRA.Num());
            PlatformData->Mips[0].BulkData.Unlock();
        }
        NewPreview->UpdateResource();

        // Swap in the new texture (TStrongObjectPtr releases the previous one for GC).
        ChannelPreviewTextures[Index].Reset(NewPreview);

        if (!ChannelBrushes[Index].IsValid())
        {
            ChannelBrushes[Index] = MakeShared<FSlateBrush>();
            ChannelBrushes[Index]->DrawAs = ESlateBrushDrawType::Image;
        }
        ChannelBrushes[Index]->SetResourceObject(NewPreview);
        ChannelBrushes[Index]->ImageSize = FVector2D(W, H);
    }

    // Cap the on-screen cell size so the 2x2 grid fits comfortably in the panel.
    const float DisplayScale = FMath::Min(1.0f, 128.0f / (float)FMath::Max(W, H));
    PreviewDisplayWidth = FMath::Max(1, FMath::RoundToInt(W * DisplayScale));
    PreviewDisplayHeight = FMath::Max(1, FMath::RoundToInt(H * DisplayScale));

    bPreviewValid = true;
}

FReply FTextureChannelUnpacker::OnExtractClicked()
{
    check(IsInGameThread());

    UTexture2D* SourceTex = SourceTexture.Get();
    if (!SourceTex)
    {
        ShowNotification(GetLocalizedMessage(
            TEXT("ErrorNoUnpackSource"),
            TEXT("Please select a source texture to unpack."),
            TEXT("アンパックするソーステクスチャを選択してください。")), false);
        return FReply::Handled();
    }

    // Collect selected channels
    TArray<int32> SelectedChannels;
    for (int32 Index = 0; Index < 4; ++Index)
    {
        if (bExportChannel[Index])
        {
            SelectedChannels.Add(Index);
        }
    }
    if (SelectedChannels.Num() == 0)
    {
        ShowNotification(GetLocalizedMessage(
            TEXT("ErrorNoChannelsSelected"),
            TEXT("Please select at least one channel to export."),
            TEXT("出力するチャンネルを少なくとも1つ選択してください。")), false);
        return FReply::Handled();
    }

    if (BaseFileName.IsEmpty())
    {
        ShowNotification(GetLocalizedMessage(
            TEXT("ErrorNoFileName"),
            TEXT("Please specify a file name."),
            TEXT("ファイル名を指定してください。")), false);
        return FReply::Handled();
    }

    // Guard against duplicate output names (possible with hand-edited preset suffixes).
    {
        TSet<FString> UniqueNames;
        for (int32 ChannelIndex : SelectedChannels)
        {
            bool bAlreadyInSet = false;
            UniqueNames.Add(GetChannelAssetName(ChannelIndex), &bAlreadyInSet);
            if (bAlreadyInSet)
            {
                ShowNotification(GetLocalizedMessage(
                    TEXT("ErrorDuplicateOutputNames"),
                    TEXT("Two or more channels resolve to the same output name. Check the preset's channel suffixes."),
                    TEXT("複数のチャンネルが同じ出力名になっています。プリセットのチャンネルサフィックスを確認してください。")), false);
                return FReply::Handled();
            }
        }
    }

#if WITH_EDITORONLY_DATA
    const int32 SrcWidth = SourceTex->Source.GetSizeX();
    const int32 SrcHeight = SourceTex->Source.GetSizeY();
#else
    const int32 SrcWidth = SourceTex->GetSizeX();
    const int32 SrcHeight = SourceTex->GetSizeY();
#endif
    if (SrcWidth < 1 || SrcHeight < 1)
    {
        ShowNotification(GetLocalizedMessage(
            TEXT("ErrorInvalidUnpackSource"),
            TEXT("The source texture has no valid source data."),
            TEXT("ソーステクスチャに有効なソースデータがありません。")), false);
        return FReply::Handled();
    }

    UE_LOG(LogTexturePacker, Log, TEXT("Unpacking Texture: %s (%d x %d), Channels: %d, Output: %s, Base Name: %s"),
        *SourceTex->GetPathName(), SrcWidth, SrcHeight, SelectedChannels.Num(), *OutputPackagePath, *BaseFileName);

    // Memory Consumption Warning Check (outputs are created at source resolution)
    if (SrcWidth > LargeTextureWarningThreshold || SrcHeight > LargeTextureWarningThreshold)
    {
        FText Msg = GetLocalizedMessage(
            TEXT("WarningHighResolution"),
            TEXT("This resolution will consume a very large amount of memory. Processing may take a long time or become unstable. Do you want to continue?"),
            TEXT("非常に大きなメモリを消費し、処理に時間がかかるか不安定になる可能性があります。続行しますか？")
        );

        EAppReturnType::Type Result = FMessageDialog::Open(EAppMsgType::YesNo, Msg);
        if (Result == EAppReturnType::No)
        {
            return FReply::Handled();
        }
    }

    // Overwrite confirmation: list every existing asset in a single dialog.
    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
    TArray<FString> ExistingNames;
    for (int32 ChannelIndex : SelectedChannels)
    {
        const FString AssetName = GetChannelAssetName(ChannelIndex);
        const FString ObjectPath = GetChannelPackageName(ChannelIndex) + TEXT(".") + AssetName;
        FAssetData ExistingAsset = AssetRegistryModule.Get().GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
        if (ExistingAsset.IsValid())
        {
            ExistingNames.Add(AssetName);
        }
    }
    if (ExistingNames.Num() > 0)
    {
        FText Msg = FText::Format(
            GetLocalizedMessage(
                TEXT("ConfirmOverwriteUnpack"),
                TEXT("The following assets already exist and will be overwritten:\n{0}\n\nDo you want to continue?"),
                TEXT("以下のアセットは既に存在するため上書きされます:\n{0}\n\n続行しますか？")
            ),
            FText::FromString(FString::Join(ExistingNames, TEXT("\n")))
        );

        EAppReturnType::Type Result = FMessageDialog::Open(EAppMsgType::YesNo, Msg);
        if (Result == EAppReturnType::No)
        {
            return FReply::Handled();
        }
    }

    // Progress: 1 extract + 1 process + one frame per saved channel.
    FScopedSlowTask SlowTask((float)(2 + SelectedChannels.Num()), GetLocalizedMessage(
        TEXT("ProgressUnpacking"),
        TEXT("Unpacking Texture..."),
        TEXT("テクスチャをアンパック中...")
    ));
    SlowTask.MakeDialog(true); // true = cancellable

    const FText CancelMsg = GetLocalizedMessage(
        TEXT("UnpackCancelled"),
        TEXT("Unpack was cancelled by user."),
        TEXT("アンパックがユーザーによってキャンセルされました。")
    );

    // ---------------------------------------------------------
    // STEP 1: Extract Raw Data from the Source (Game Thread)
    // ---------------------------------------------------------
    SlowTask.EnterProgressFrame(1.0f, GetLocalizedMessage(
        TEXT("ProgressExtracting"),
        TEXT("Extracting source data..."),
        TEXT("ソースデータを抽出中...")
    ));

    FTextureRawData Raw = ExtractTextureSourceData(SourceTex);
    if (!Raw.bIsValid)
    {
        ShowNotification(!Raw.ErrorMessage.IsEmpty() ? Raw.ErrorMessage : GetLocalizedMessage(
            TEXT("ErrorUnpackExtractFailed"),
            TEXT("Failed to read source texture data."),
            TEXT("ソーステクスチャのデータを読み取れませんでした。")), false);
        return FReply::Handled();
    }

    if (SlowTask.ShouldCancel())
    {
        ShowNotification(CancelMsg, false);
        return FReply::Handled();
    }

    // ---------------------------------------------------------
    // STEP 2: Extract Channels in Parallel (Background Threads)
    // ---------------------------------------------------------
    SlowTask.EnterProgressFrame(1.0f, GetLocalizedMessage(
        TEXT("ProgressProcessingChannels"),
        TEXT("Extracting channels..."),
        TEXT("チャンネルを抽出中...")
    ));

    TArray<FTextureProcessResult> Results;
    Results.SetNum(SelectedChannels.Num());

    // The raw data is shared between the tasks, so pass bCanConsumeInput=false
    // to keep every task read-only.
    ParallelFor(SelectedChannels.Num(), [&Raw, &Results, &SelectedChannels](int32 Index)
    {
        Results[Index] = ProcessTextureSourceData(Raw, Raw.Width, Raw.Height, GUnpackChannelOrder[SelectedChannels[Index]], /*bCanConsumeInput=*/false);
    });

    // A single shared source either succeeds for all channels or fails for all
    // (e.g. unsupported format), so report the first error and abort.
    for (const FTextureProcessResult& Result : Results)
    {
        if (!Result.bSuccess)
        {
            if (!Result.ErrorMessage.IsEmpty())
            {
                ShowNotification(Result.ErrorMessage, false);
            }
            return FReply::Handled();
        }
    }

    if (SlowTask.ShouldCancel())
    {
        ShowNotification(CancelMsg, false);
        return FReply::Handled();
    }

    // ---------------------------------------------------------
    // STEP 3: Write One Grayscale Asset per Channel (Game Thread)
    // ---------------------------------------------------------
    int32 SavedCount = 0;
    bool bCancelled = false;

    for (int32 SelectionIndex = 0; SelectionIndex < SelectedChannels.Num(); ++SelectionIndex)
    {
        const int32 ChannelIndex = SelectedChannels[SelectionIndex];
        const FString AssetName = GetChannelAssetName(ChannelIndex);
        const FString PackageName = GetChannelPackageName(ChannelIndex);

        SlowTask.EnterProgressFrame(1.0f, FText::Format(
            GetLocalizedMessage(TEXT("ProgressSavingChannel"), TEXT("Saving {0}..."), TEXT("{0} を保存中...")),
            FText::FromString(AssetName)
        ));

        if (SlowTask.ShouldCancel())
        {
            bCancelled = true;
            break;
        }

        const TArray<uint8>& Data = Results[SelectionIndex].ProcessedData;
        if (Data.Num() != Raw.Width * Raw.Height)
        {
            UE_LOG(LogTexturePacker, Error, TEXT("Unexpected channel data size for %s (%d, expected %d). Skipping."),
                *AssetName, Data.Num(), Raw.Width * Raw.Height);
            continue;
        }

        TStrongObjectPtr<UPackage> PackagePtr(CreatePackage(*PackageName));
        UPackage* Package = PackagePtr.Get();
        if (!Package)
        {
            ShowNotification(GetLocalizedMessage(
                TEXT("ErrorPackageCreation"),
                TEXT("Failed to create package."),
                TEXT("パッケージの作成に失敗しました。")), false);
            continue;
        }
        Package->FullyLoad();

        UTexture2D* NewTexture = NewObject<UTexture2D>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_MarkAsRootSet);

#if WITH_EDITORONLY_DATA
        NewTexture->Source.Init(Raw.Width, Raw.Height, 1, 1, TSF_G8);
        uint8* MipData = NewTexture->Source.LockMip(0);
        if (MipData)
        {
            FMemory::Memcpy(MipData, Data.GetData(), Data.Num());
        }
        NewTexture->Source.UnlockMip(0);
#endif

        // Extracted channels are data, not color: grayscale compression, linear color space.
        NewTexture->CompressionSettings = TC_Grayscale;
        NewTexture->SRGB = false;

        NewTexture->UpdateResource();
        NewTexture->PostEditChange();

        Package->MarkPackageDirty();
        FAssetRegistryModule::AssetCreated(NewTexture);
        ++SavedCount;
    }

    if (bCancelled)
    {
        ShowNotification(CancelMsg, false);
    }
    else if (SavedCount > 0)
    {
        ShowNotification(FText::Format(
            GetLocalizedMessage(
                TEXT("SuccessUnpacked"),
                TEXT("Unpacked {0} texture(s) to {1}"),
                TEXT("{0} 枚のテクスチャを {1} にアンパックしました")
            ),
            FText::AsNumber(SavedCount),
            FText::FromString(OutputPackagePath)), true);
    }
    else
    {
        ShowNotification(GetLocalizedMessage(
            TEXT("ErrorUnpackNothingSaved"),
            TEXT("No textures could be saved."),
            TEXT("テクスチャを保存できませんでした。")), false);
    }

    return FReply::Handled();
}

TSharedRef<SWidget> FTextureChannelUnpacker::CreateChannelCell(int32 ChannelIndex)
{
    return SNew(SVerticalBox)

        // Header row: export checkbox + channel label + uniform badge
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(0.0f, 0.0f, 0.0f, 2.0f)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot()
            .AutoWidth()
            .VAlign(VAlign_Center)
            [
                SNew(SCheckBox)
                .ToolTipText(GetLocalizedMessage(
                    TEXT("ExportChannelTooltip"),
                    TEXT("Export this channel as a grayscale texture."),
                    TEXT("このチャンネルをグレースケールテクスチャとして出力します。")
                ))
                .IsChecked_Lambda([this, ChannelIndex]()
                {
                    return bExportChannel[ChannelIndex] ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
                })
                .OnCheckStateChanged_Lambda([this, ChannelIndex](ECheckBoxState NewState)
                {
                    bExportChannel[ChannelIndex] = (NewState == ECheckBoxState::Checked);
                })
            ]
            + SHorizontalBox::Slot()
            .AutoWidth()
            .VAlign(VAlign_Center)
            .Padding(4.0f, 0.0f, 0.0f, 0.0f)
            [
                SNew(STextBlock)
                .Text_Lambda([this, ChannelIndex]()
                {
                    return GetChannelHeaderText(ChannelIndex);
                })
                .Font(FAppStyle::GetFontStyle("PropertyWindow.BoldFont"))
            ]
            + SHorizontalBox::Slot()
            .FillWidth(1.0f)
            [
                SNew(SSpacer)
            ]
            + SHorizontalBox::Slot()
            .AutoWidth()
            .VAlign(VAlign_Center)
            [
                // "Possibly unused" badge for uniform channels
                SNew(STextBlock)
                .Visibility_Lambda([this, ChannelIndex]()
                {
                    return (bPreviewValid && bChannelUniform[ChannelIndex]) ? EVisibility::Visible : EVisibility::Collapsed;
                })
                .Text_Lambda([this, ChannelIndex]()
                {
                    return FText::Format(
                        GetLocalizedMessage(TEXT("UniformChannelBadge"), TEXT("Uniform ({0})"), TEXT("均一 ({0})")),
                        FText::AsNumber(ChannelUniformValue[ChannelIndex]));
                })
                .ToolTipText(GetLocalizedMessage(
                    TEXT("UniformChannelTooltip"),
                    TEXT("Every pixel in this channel has the same value, so it is likely unused. It has been excluded from export automatically; re-check the box to export it anyway."),
                    TEXT("このチャンネルは全ピクセルが同じ値のため、未使用の可能性があります。自動的に出力対象から除外されました。出力したい場合はチェックを入れ直してください。")
                ))
                .ColorAndOpacity(FSlateColor(FLinearColor(1.0f, 0.75f, 0.25f)))
                .Font(FAppStyle::GetFontStyle("PropertyWindow.NormalFont"))
            ]
        ]

        // Channel preview image
        + SVerticalBox::Slot()
        .AutoHeight()
        .HAlign(HAlign_Left)
        [
            SNew(SBox)
            .WidthOverride_Lambda([this]() -> FOptionalSize
            {
                return FOptionalSize((float)PreviewDisplayWidth);
            })
            .HeightOverride_Lambda([this]() -> FOptionalSize
            {
                return FOptionalSize((float)PreviewDisplayHeight);
            })
            [
                SNew(SBorder)
                .BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
                .Padding(0.0f)
                [
                    SNew(SImage)
                    .Image_Lambda([this, ChannelIndex]() -> const FSlateBrush*
                    {
                        return ChannelBrushes[ChannelIndex].IsValid() ? ChannelBrushes[ChannelIndex].Get() : nullptr;
                    })
                ]
            ]
        ]

        // Output asset name preview
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(0.0f, 2.0f, 0.0f, 0.0f)
        [
            SNew(STextBlock)
            .Text_Lambda([this, ChannelIndex]()
            {
                return FText::FromString(GetChannelAssetName(ChannelIndex));
            })
            .ToolTipText(GetLocalizedMessage(
                TEXT("OutputNamePreviewTooltip"),
                TEXT("Name of the asset that will be created for this channel."),
                TEXT("このチャンネルから作成されるアセット名です。")
            ))
            .ColorAndOpacity(FSlateColor::UseSubduedForeground())
            .Font(FAppStyle::GetFontStyle("PropertyWindow.NormalFont"))
        ];
}

TSharedRef<SWidget> FTextureChannelUnpacker::CreateContent()
{
    // Path picker button (same pattern as the Pack tab)
    TSharedRef<SComboButton> PathPickerComboButton = SNew(SComboButton)
        .ContentPadding(FMargin(2.0f, 2.0f))
        .ButtonContent()
        [
            SNew(SImage)
            .Image(FAppStyle::GetBrush("Icons.FolderClosed"))
        ];

    TWeakPtr<SComboButton> WeakComboButton = PathPickerComboButton;
    PathPickerComboButton->SetOnGetMenuContent(FOnGetContent::CreateLambda([this, WeakComboButton]()
    {
        FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
        FPathPickerConfig PathPickerConfig;
        PathPickerConfig.DefaultPath = OutputPackagePath;
        PathPickerConfig.OnPathSelected = FOnPathSelected::CreateLambda([this, WeakComboButton](const FString& NewPath)
        {
            OutputPackagePath = NewPath;
            if (TSharedPtr<SComboButton> StrongComboButton = WeakComboButton.Pin())
            {
                StrongComboButton->SetIsOpen(false);
            }
        });

        return ContentBrowserModule.Get().CreatePathPicker(PathPickerConfig);
    }));

    return SNew(SVerticalBox)

        // Scrollable content area
        + SVerticalBox::Slot()
        .FillHeight(1.0f)
        [
            SNew(SScrollBox)

            // Preset Selection (drives per-channel output suffixes)
            + SScrollBox::Slot()
            .Padding(10.0f)
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot()
                .AutoHeight()
                .Padding(0.0f, 0.0f, 0.0f, 4.0f)
                [
                    SNew(STextBlock)
                    .Text(GetLocalizedMessage(TEXT("PresetLabel"), TEXT("Preset"), TEXT("プリセット")))
                    .Font(FAppStyle::GetFontStyle("PropertyWindow.BoldFont"))
                ]
                + SVerticalBox::Slot()
                .AutoHeight()
                [
                    SAssignNew(PresetComboBox, SComboBox<TSharedPtr<FChannelPackerPreset>>)
                    .ToolTipText(GetLocalizedMessage(
                        TEXT("UnpackPresetTooltip"),
                        TEXT("Determines the output name suffix for each channel (e.g. _AO / _Roughness / _Metallic for ORM). Presets are managed in the Pack tab."),
                        TEXT("各チャンネルの出力名サフィックスを決定します（例: ORM の場合 _AO / _Roughness / _Metallic）。プリセットの管理はパックタブで行えます。")
                    ))
                    .OptionsSource(Presets)
                    .OnSelectionChanged_Lambda([this](TSharedPtr<FChannelPackerPreset> NewSelection, ESelectInfo::Type SelectInfo)
                    {
                        if (NewSelection.IsValid() && SelectInfo != ESelectInfo::Direct)
                        {
                            ApplyPreset(NewSelection);
                        }
                    })
                    .OnGenerateWidget_Lambda([](TSharedPtr<FChannelPackerPreset> Item)
                    {
                        return SNew(STextBlock).Text(Item->GetDisplayName());
                    })
                    [
                        SNew(STextBlock)
                        .Text_Lambda([this]()
                        {
                            return CurrentPreset.IsValid() ? CurrentPreset->GetDisplayName() : FText::GetEmpty();
                        })
                    ]
                ]
            ]

            // Separator
            + SScrollBox::Slot()
            .Padding(10.0f, 5.0f)
            [
                SNew(SSeparator)
            ]

            // Source Texture
            + SScrollBox::Slot()
            .Padding(10.0f)
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot()
                .AutoHeight()
                .Padding(0.0f, 0.0f, 0.0f, 4.0f)
                [
                    SNew(STextBlock)
                    .Text(GetLocalizedMessage(TEXT("UnpackSourceLabel"), TEXT("Source Texture"), TEXT("ソーステクスチャ")))
                    .ToolTipText(GetLocalizedMessage(
                        TEXT("UnpackSourceTooltip"),
                        TEXT("The packed texture whose channels will be extracted into separate grayscale assets. Its R/G/B/A channels are previewed as soon as it is selected."),
                        TEXT("チャンネルを個別のグレースケールアセットとして抽出するパック済みテクスチャです。選択するとすぐに R/G/B/A の内容がプレビュー表示されます。")
                    ))
                    .Font(FAppStyle::GetFontStyle("PropertyWindow.BoldFont"))
                ]
                + SVerticalBox::Slot()
                .AutoHeight()
                [
                    SNew(SObjectPropertyEntryBox)
                    .AllowedClass(UTexture2D::StaticClass())
                    .ObjectPath_Lambda([this]()
                    {
                        return SourceTexture.IsValid() ? SourceTexture->GetPathName() : FString();
                    })
                    .OnObjectChanged_Lambda([this](const FAssetData& AssetData)
                    {
                        OnSourceTextureChanged(Cast<UTexture2D>(AssetData.GetAsset()));
                    })
                    .AllowClear(true)
                    .DisplayThumbnail(true)
                    .ThumbnailPool(UThumbnailManager::Get().GetSharedThumbnailPool())
                ]
            ]

            // Separator
            + SScrollBox::Slot()
            .Padding(10.0f, 5.0f)
            [
                SNew(SSeparator)
            ]

            // Channel preview grid
            + SScrollBox::Slot()
            .Padding(10.0f, 5.0f)
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot()
                .AutoHeight()
                .Padding(0.0f, 0.0f, 0.0f, 6.0f)
                [
                    SNew(STextBlock)
                    .Text(GetLocalizedMessage(TEXT("UnpackChannelsLabel"), TEXT("Channels"), TEXT("チャンネル")))
                    .Font(FAppStyle::GetFontStyle("PropertyWindow.BoldFont"))
                ]
                + SVerticalBox::Slot()
                .AutoHeight()
                .HAlign(HAlign_Center)
                [
                    SNew(SUniformGridPanel)
                    .SlotPadding(FMargin(6.0f))
                    .Visibility_Lambda([this]()
                    {
                        return bPreviewValid ? EVisibility::Visible : EVisibility::Collapsed;
                    })
                    + SUniformGridPanel::Slot(0, 0)[ CreateChannelCell(0) ]
                    + SUniformGridPanel::Slot(1, 0)[ CreateChannelCell(1) ]
                    + SUniformGridPanel::Slot(0, 1)[ CreateChannelCell(2) ]
                    + SUniformGridPanel::Slot(1, 1)[ CreateChannelCell(3) ]
                ]
                + SVerticalBox::Slot()
                .AutoHeight()
                .HAlign(HAlign_Center)
                [
                    // Hint text (visible before a source is selected)
                    SNew(STextBlock)
                    .Visibility_Lambda([this]()
                    {
                        return bPreviewValid ? EVisibility::Collapsed : EVisibility::Visible;
                    })
                    .Text(GetLocalizedMessage(
                        TEXT("UnpackPreviewHint"),
                        TEXT("Select a source texture to preview its channels."),
                        TEXT("ソーステクスチャを選択するとチャンネル内容を表示します。")
                    ))
                    .ColorAndOpacity(FSlateColor::UseSubduedForeground())
                    .Font(FAppStyle::GetFontStyle("PropertyWindow.NormalFont"))
                ]
            ]

            // Separator
            + SScrollBox::Slot()
            .Padding(10.0f, 5.0f)
            [
                SNew(SSeparator)
            ]

            // Output Settings Header
            + SScrollBox::Slot()
            .Padding(10.0f, 5.0f)
            [
                SNew(STextBlock)
                .Text(LOCTEXT("OutputSettingsLabel", "Output Settings"))
                .Font(FAppStyle::GetFontStyle("PropertyWindow.BoldFont"))
            ]

            // Output Path
            + SScrollBox::Slot()
            .Padding(10.0f, 5.0f)
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot()
                .AutoHeight()
                .Padding(0.0f, 0.0f, 0.0f, 4.0f)
                [
                    SNew(STextBlock)
                    .Text(LOCTEXT("OutputPathLabel", "Output Path (e.g. /Game/...)"))
                    .Font(FAppStyle::GetFontStyle("PropertyWindow.NormalFont"))
                ]
                + SVerticalBox::Slot()
                .AutoHeight()
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot()
                    .FillWidth(1.0f)
                    .HAlign(HAlign_Fill)
                    [
                        SNew(SEditableTextBox)
                        .Text_Lambda([this] { return FText::FromString(OutputPackagePath); })
                        .OnTextCommitted_Lambda([this](const FText& NewText, ETextCommit::Type) { OutputPackagePath = NewText.ToString(); })
                    ]
                    + SHorizontalBox::Slot()
                    .AutoWidth()
                    [
                        PathPickerComboButton
                    ]
                ]
            ]

            // Base File Name
            + SScrollBox::Slot()
            .Padding(10.0f, 5.0f)
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot()
                .AutoHeight()
                .Padding(0.0f, 0.0f, 0.0f, 4.0f)
                [
                    SNew(STextBlock)
                    .Text(GetLocalizedMessage(TEXT("BaseFileNameLabel"), TEXT("Base File Name"), TEXT("ベースファイル名")))
                    .Font(FAppStyle::GetFontStyle("PropertyWindow.NormalFont"))
                ]
                + SVerticalBox::Slot()
                .AutoHeight()
                [
                    SNew(SEditableTextBox)
                    .Text_Lambda([this] { return FText::FromString(BaseFileName); })
                    .OnTextCommitted_Lambda([this](const FText& NewText, ETextCommit::Type CommitType)
                    {
                        BaseFileName = NewText.ToString();
                        if (CommitType == ETextCommit::OnEnter || CommitType == ETextCommit::OnUserMovedFocus)
                        {
                            bBaseNameManuallyEdited = true;
                        }
                    })
                ]
                + SVerticalBox::Slot()
                .AutoHeight()
                .Padding(0.0f, 4.0f, 0.0f, 0.0f)
                [
                    SNew(STextBlock)
                    .Text(GetLocalizedMessage(
                        TEXT("BaseFileNameHint"),
                        TEXT("Per-channel suffixes are appended automatically (e.g. _AO)."),
                        TEXT("チャンネルごとのサフィックス（例: _AO）が自動的に付加されます。")
                    ))
                    .ColorAndOpacity(FSlateColor::UseSubduedForeground())
                    .Font(FAppStyle::GetFontStyle("PropertyWindow.NormalFont"))
                ]
            ]
        ] // end SScrollBox

        // Unpack Button (pinned at bottom, always visible)
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(20.0f)
        .HAlign(HAlign_Fill)
        [
            SNew(SButton)
            .HAlign(HAlign_Center)
            .VAlign(VAlign_Center)
            .ContentPadding(FMargin(0.0f, 10.0f))
            .OnClicked_Lambda([this]()
            {
                return OnExtractClicked();
            })
            [
                SNew(STextBlock)
                .Text(GetLocalizedMessage(TEXT("UnpackButtonText"), TEXT("Unpack Textures"), TEXT("テクスチャをアンパック")))
                .Font(FAppStyle::GetFontStyle("PropertyWindow.BoldFont"))
            ]
        ];
}

#undef LOCTEXT_NAMESPACE
