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

/** Channel labels for the unpack grid, indexed R=0, G=1, B=2, A=3. */
static const TCHAR* GUnpackChannelLetters[4] = { TEXT("R"), TEXT("G"), TEXT("B"), TEXT("A") };

/**
 * @brief Builds a per-pixel channel sampler for the given source format and hands it to Visitor.
 *
 * The sampler reads a single channel value straight out of the locked mip and converts it to
 * 8-bit, so neither the preview nor the extraction needs an intermediate full-resolution copy
 * or FColor buffer. Dispatching on the format once (outside the pixel loop) keeps the inner
 * loop free of per-pixel branching on the format.
 *
 * Sampler signature: uint8 (int64 PixelIndex, int32 ChannelIndex), where ChannelIndex is
 * 0=R, 1=G, 2=B, 3=A. Pixel indices are 64-bit, so sources larger than 2 GB are handled.
 *
 * Single-channel formats (G8/G16/R16F/R32F) replicate their lone value across R/G/B and
 * report an opaque (255) Alpha, matching how the packing pipeline widens them to FColor.
 *
 * @return False if the source format is unsupported (Visitor is then not called).
 */
template <typename FVisitor>
static bool VisitChannelSampler(const uint8* Src, ETextureSourceFormat Format, FVisitor&& Visitor)
{
    // Byte offsets of R, G, B, A within a BGRA8 pixel (see GetBGRAChannelOffset).
    static const int32 BGRAOffsets[4] = { 2, 1, 0, 3 };

    switch (Format)
    {
    case TSF_BGRA8:
    {
        Visitor([Src](int64 PixelIndex, int32 Channel) -> uint8
        {
            return Src[PixelIndex * 4 + BGRAOffsets[Channel]];
        });
        return true;
    }
    case TSF_G8:
    {
        Visitor([Src](int64 PixelIndex, int32 Channel) -> uint8
        {
            return Channel == 3 ? (uint8)255 : Src[PixelIndex];
        });
        return true;
    }
    case TSF_G16:
    {
        const uint16* Pixels = (const uint16*)Src;
        Visitor([Pixels](int64 PixelIndex, int32 Channel) -> uint8
        {
            return Channel == 3 ? (uint8)255 : (uint8)(Pixels[PixelIndex] >> 8);
        });
        return true;
    }
    case TSF_R16F:
    {
        const FFloat16* Pixels = (const FFloat16*)Src;
        Visitor([Pixels](int64 PixelIndex, int32 Channel) -> uint8
        {
            if (Channel == 3)
            {
                return 255;
            }
            return (uint8)FMath::Clamp<float>((float)Pixels[PixelIndex] * 255.0f, 0.0f, 255.0f);
        });
        return true;
    }
    case TSF_R32F:
    {
        const float* Pixels = (const float*)Src;
        Visitor([Pixels](int64 PixelIndex, int32 Channel) -> uint8
        {
            if (Channel == 3)
            {
                return 255;
            }
            return (uint8)FMath::Clamp<float>(Pixels[PixelIndex] * 255.0f, 0.0f, 255.0f);
        });
        return true;
    }
    case TSF_RGBA32F:
    {
        const FLinearColor* Pixels = (const FLinearColor*)Src;
        Visitor([Pixels](int64 PixelIndex, int32 Channel) -> uint8
        {
            const FLinearColor& LC = Pixels[PixelIndex];
            float Value;
            switch (Channel)
            {
            case 0:  Value = LC.R; break;
            case 1:  Value = LC.G; break;
            case 2:  Value = LC.B; break;
            default: Value = LC.A; break;
            }
            return (uint8)FMath::Clamp<float>(Value * 255.0f, 0.0f, 255.0f);
        });
        return true;
    }
    default:
        return false;
    }
}

/** Per-destination-row uniform-tracking state, merged after the parallel scan. */
struct FChannelRowScan
{
    uint8 First[4] = { 0, 0, 0, 0 };
    bool bUniform[4] = { true, true, true, true };
};

/**
 * @brief Box-downsamples all four channels and detects uniform channels in a single pass.
 *
 * Previously the preview converted the whole source to FColor once per channel (four
 * full-resolution buffers for a 256 px thumbnail) and then scanned the source again per
 * channel for uniform detection. This walks every source pixel exactly once instead,
 * accumulating into the small destination buffers, so the only allocations are the four
 * DstW*DstH byte arrays regardless of how large the source is.
 *
 * Uniform detection stays exact because every source pixel is visited — averaging into the
 * preview could otherwise hide a single differing pixel.
 *
 * Work is split across destination rows. Each row owns a disjoint band of source rows and
 * writes only its own slice of the output, so no synchronization is needed.
 */
template <typename FSampler>
static void ScanAndDownsampleChannels(
    const FSampler& Sample,
    int32 SrcW, int32 SrcH,
    int32 DstW, int32 DstH,
    TArray<uint8> (&OutChannels)[4],
    bool (&bOutUniform)[4],
    uint8 (&OutUniformValue)[4])
{
    const int32 NumDst = DstW * DstH;
    for (int32 Channel = 0; Channel < 4; ++Channel)
    {
        OutChannels[Channel].SetNumUninitialized(NumDst);
    }

    TArray<FChannelRowScan> RowScans;
    RowScans.SetNum(DstH);

    ParallelFor(DstH, [&](int32 dy)
    {
        // Source rows covered by this destination row. DstH <= SrcH, so the band is never empty
        // and the bands of all destination rows exactly partition the source.
        const int64 y0 = ((int64)dy * SrcH) / DstH;
        const int64 y1 = FMath::Max(y0 + 1, ((int64)(dy + 1) * SrcH) / DstH);

        FChannelRowScan& Row = RowScans[dy];
        bool bFirstPixel = true;

        for (int32 dx = 0; dx < DstW; ++dx)
        {
            const int64 x0 = ((int64)dx * SrcW) / DstW;
            const int64 x1 = FMath::Max(x0 + 1, ((int64)(dx + 1) * SrcW) / DstW);

            uint64 Sums[4] = { 0, 0, 0, 0 };
            uint64 Count = 0;

            for (int64 y = y0; y < y1; ++y)
            {
                const int64 RowOffset = y * (int64)SrcW;
                for (int64 x = x0; x < x1; ++x)
                {
                    const int64 SrcIndex = RowOffset + x;
                    for (int32 Channel = 0; Channel < 4; ++Channel)
                    {
                        const uint8 Value = Sample(SrcIndex, Channel);
                        Sums[Channel] += Value;
                        if (bFirstPixel)
                        {
                            Row.First[Channel] = Value;
                        }
                        else if (Row.bUniform[Channel] && Value != Row.First[Channel])
                        {
                            Row.bUniform[Channel] = false;
                        }
                    }
                    bFirstPixel = false;
                    ++Count;
                }
            }

            const int32 DstIndex = dy * DstW + dx;
            for (int32 Channel = 0; Channel < 4; ++Channel)
            {
                OutChannels[Channel][DstIndex] = (uint8)(Sums[Channel] / FMath::Max<uint64>(Count, 1));
            }
        }
    });

    // A channel is uniform overall only if every row is uniform and they all agree on the value.
    for (int32 Channel = 0; Channel < 4; ++Channel)
    {
        OutUniformValue[Channel] = RowScans[0].First[Channel];
        bOutUniform[Channel] = true;
        for (int32 dy = 0; dy < DstH; ++dy)
        {
            if (!RowScans[dy].bUniform[Channel] || RowScans[dy].First[Channel] != OutUniformValue[Channel])
            {
                bOutUniform[Channel] = false;
                break;
            }
        }
    }
}

/**
 * @brief Copies one channel out of the source at full resolution.
 *
 * Reads straight from the locked mip through the sampler, so the only allocation is the
 * output buffer itself (one byte per pixel).
 */
template <typename FSampler>
static void ExtractChannelBytes(const FSampler& Sample, int32 Channel, int64 NumPixels, TArray<uint8>& Out)
{
    Out.SetNumUninitialized((int32)NumPixels);
    uint8* Dest = Out.GetData();

    ParallelFor((int32)NumPixels, [&Sample, Dest, Channel](int32 PixelIndex)
    {
        Dest[PixelIndex] = Sample((int64)PixelIndex, Channel);
    });
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

#if WITH_EDITORONLY_DATA
    const int32 SrcW = SourceTex->Source.GetSizeX();
    const int32 SrcH = SourceTex->Source.GetSizeY();
    const ETextureSourceFormat Format = SourceTex->Source.GetFormat();

    if (SrcW < 1 || SrcH < 1)
    {
        ShowNotification(GetLocalizedMessage(
            TEXT("ErrorInvalidUnpackSource"),
            TEXT("The source texture has no valid source data."),
            TEXT("ソーステクスチャに有効なソースデータがありません。")), false);
        return;
    }

    // Derive a small preview resolution from the source aspect ratio so the preview stays
    // cheap to build even for very large sources (e.g. 16K).
    const int32 PreviewMaxDim = 256;
    const float Scale = FMath::Min(1.0f, (float)PreviewMaxDim / (float)FMath::Max(SrcW, SrcH));
    const int32 W = FMath::Max(1, FMath::RoundToInt(SrcW * Scale));
    const int32 H = FMath::Max(1, FMath::RoundToInt(SrcH * Scale));
    const int32 NumPixels = W * H;

    // Read straight from the locked mip: no full-resolution copy and no FColor buffers, so
    // selecting a large texture costs only the four small preview arrays. One pass produces
    // both the downsampled previews and the exact uniform-channel verdicts.
    uint8* Locked = SourceTex->Source.LockMip(0);
    if (!Locked)
    {
        ShowNotification(GetLocalizedMessage(
            TEXT("ErrorLockFailed"),
            TEXT("Failed to access texture data. The texture may be corrupted or in use. Try reimporting the texture."),
            TEXT("テクスチャデータへのアクセスに失敗しました。テクスチャが破損しているか、使用中の可能性があります。テクスチャを再インポートしてください。")), false);
        return;
    }

    TArray<uint8> Channels[4];
    bool bUniform[4] = { false, false, false, false };
    uint8 UniformValue[4] = { 0, 0, 0, 0 };

    const bool bFormatSupported = VisitChannelSampler(Locked, Format, [&](auto&& Sample)
    {
        ScanAndDownsampleChannels(Sample, SrcW, SrcH, W, H, Channels, bUniform, UniformValue);
    });

    SourceTex->Source.UnlockMip(0);

    if (!bFormatSupported)
    {
        UE_LOG(LogTexturePacker, Error, TEXT("Unsupported Source Format: %d for texture: %s"), (int32)Format, *SourceTex->GetName());
        ShowNotification(GetLocalizedMessage(
            TEXT("ErrorUnsupportedFormat"),
            TEXT("Texture format not supported. Please convert to PNG or TGA."),
            TEXT("テクスチャ形式がサポートされていません。PNGまたはTGAに変換してください。")), false);
        return;
    }

    // Channels that hold a single value are likely unused, so they are excluded from export
    // by default (the user can re-check the box to export them anyway).
    for (int32 Index = 0; Index < 4; ++Index)
    {
        bChannelUniform[Index] = bUniform[Index];
        ChannelUniformValue[Index] = UniformValue[Index];
        bExportChannel[Index] = !bUniform[Index];
    }

    // Build one grayscale transient texture per channel.
    for (int32 Index = 0; Index < 4; ++Index)
    {
        const TArray<uint8>& Data = Channels[Index];

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
#else
    UE_LOG(LogTexturePacker, Error, TEXT("TextureChannelPacker requires WITH_EDITORONLY_DATA to access Source."));
#endif
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

    // Progress: 1 extract + one frame per saved channel.
    FScopedSlowTask SlowTask((float)(1 + SelectedChannels.Num()), GetLocalizedMessage(
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
    // STEP 1: Extract the Selected Channels
    // ---------------------------------------------------------
    SlowTask.EnterProgressFrame(1.0f, GetLocalizedMessage(
        TEXT("ProgressProcessingChannels"),
        TEXT("Extracting channels..."),
        TEXT("チャンネルを抽出中...")
    ));

    // Output resolution always matches the source, so no resize (and therefore no FColor
    // conversion) is needed: each channel is read straight out of the locked mip. The mip
    // stays locked across the extraction, which costs nothing extra in memory — only the
    // one-byte-per-pixel output buffers are allocated.
    TArray<TArray<uint8>> ChannelData;
    ChannelData.SetNum(SelectedChannels.Num());

    // Set once the channels have actually been read. Stays false in non-editor builds, where
    // Source is unavailable; checked after the guarded block so neither path has dead code.
    bool bChannelsExtracted = false;

#if WITH_EDITORONLY_DATA
    const ETextureSourceFormat SourceFormat = SourceTex->Source.GetFormat();
    const int64 NumSourcePixels = (int64)SrcWidth * (int64)SrcHeight;

    uint8* Locked = SourceTex->Source.LockMip(0);
    if (!Locked)
    {
        ShowNotification(GetLocalizedMessage(
            TEXT("ErrorLockFailed"),
            TEXT("Failed to access texture data. The texture may be corrupted or in use. Try reimporting the texture."),
            TEXT("テクスチャデータへのアクセスに失敗しました。テクスチャが破損しているか、使用中の可能性があります。テクスチャを再インポートしてください。")), false);
        return FReply::Handled();
    }

    const bool bFormatSupported = VisitChannelSampler(Locked, SourceFormat, [&](auto&& Sample)
    {
        // One channel at a time; each extraction parallelizes internally over pixels.
        for (int32 Index = 0; Index < SelectedChannels.Num(); ++Index)
        {
            ExtractChannelBytes(Sample, SelectedChannels[Index], NumSourcePixels, ChannelData[Index]);
        }
    });

    SourceTex->Source.UnlockMip(0);

    if (!bFormatSupported)
    {
        UE_LOG(LogTexturePacker, Error, TEXT("Unsupported Source Format: %d for texture: %s"), (int32)SourceFormat, *SourceTex->GetName());
        ShowNotification(GetLocalizedMessage(
            TEXT("ErrorUnsupportedFormat"),
            TEXT("Texture format not supported. Please convert to PNG or TGA."),
            TEXT("テクスチャ形式がサポートされていません。PNGまたはTGAに変換してください。")), false);
        return FReply::Handled();
    }

    bChannelsExtracted = true;
#endif

    if (!bChannelsExtracted)
    {
        UE_LOG(LogTexturePacker, Error, TEXT("TextureChannelPacker requires WITH_EDITORONLY_DATA to access Source."));
        ShowNotification(GetLocalizedMessage(
            TEXT("ErrorNoEditorData"),
            TEXT("This plugin requires Editor-only data to function. Ensure the project is built with editor support."),
            TEXT("このプラグインはエディター専用データが必要です。プロジェクトがエディターサポート付きでビルドされていることを確認してください。")), false);
        return FReply::Handled();
    }

    if (SlowTask.ShouldCancel())
    {
        ShowNotification(CancelMsg, false);
        return FReply::Handled();
    }

    // ---------------------------------------------------------
    // STEP 2: Write One Grayscale Asset per Channel (Game Thread)
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

        const TArray<uint8>& Data = ChannelData[SelectionIndex];
        if ((int64)Data.Num() != (int64)SrcWidth * (int64)SrcHeight)
        {
            UE_LOG(LogTexturePacker, Error, TEXT("Unexpected channel data size for %s (%d, expected %lld). Skipping."),
                *AssetName, Data.Num(), (int64)SrcWidth * (int64)SrcHeight);
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
        NewTexture->Source.Init(SrcWidth, SrcHeight, 1, 1, TSF_G8);
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
