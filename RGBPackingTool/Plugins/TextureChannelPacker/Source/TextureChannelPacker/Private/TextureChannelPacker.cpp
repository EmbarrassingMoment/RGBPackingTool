#include "TextureChannelPacker.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SSpacer.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Styling/AppStyle.h"
#include "Logging/LogMacros.h"
#include "PropertyCustomizationHelpers.h"
#include "Engine/Texture2D.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/Paths.h"
#include "ImageUtils.h"
#include "Math/UnrealMathUtility.h"
#include "Math/Float16.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Images/SImage.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "Internationalization/Internationalization.h"
#include "Internationalization/Culture.h"

#define LOCTEXT_NAMESPACE "FTextureChannelPackerModule"

DEFINE_LOG_CATEGORY_STATIC(LogTexturePacker, Log, All);

static const FName TextureChannelPackerTabName("TextureChannelPacker");

// Helper function to localize notifications
static FText GetLocalizedMessage(const FString& Key, const FString& EnglishText, const FString& JapaneseText)
{
    FString CultureName = FInternationalization::Get().GetCurrentCulture()->GetTwoLetterISOLanguageName();
    if (CultureName == TEXT("ja"))
    {
        return FText::FromString(JapaneseText);
    }
    // We return FText::FromString to avoid unsafe usage of internal localization macros with dynamic strings.
    return FText::FromString(EnglishText);
}

void FTextureChannelPackerModule::StartupModule()
{
    // Initialize Compression Options
    CompressionOptions.Add(MakeShared<FString>("Masks (Recommended)"));
    CompressionOptions.Add(MakeShared<FString>("Grayscale"));
    CompressionOptions.Add(MakeShared<FString>("Default"));
    CurrentCompressionOption = CompressionOptions[0];

    // Register Nomad Tab
    FGlobalTabmanager::Get()->RegisterNomadTabSpawner(TextureChannelPackerTabName, FOnSpawnTab::CreateRaw(this, &FTextureChannelPackerModule::OnSpawnPluginTab))
        .SetDisplayName(LOCTEXT("TextureChannelPackerTabTitle", "Texture Channel Packer"))
        .SetMenuType(ETabSpawnerMenuType::Hidden)
        .SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Layout"));

    // This code will execute after your module is loaded into memory; the exact timing is specified in the .uplugin file format
    FToolMenuOwnerScoped OwnerScoped(this);
    UToolMenus* ToolMenus = UToolMenus::Get();

    // Find the 'Tools' menu in the level editor main menu
    UToolMenu* ToolsMenu = ToolMenus->ExtendMenu("LevelEditor.MainMenu.Tools");

    // Add a new section to the 'Tools' menu
    FToolMenuSection& Section = ToolsMenu->AddSection("TextureChannelPacker", LOCTEXT("TextureChannelPackerSection", "Texture Packing"));

    // Add a new menu entry to the new section
    Section.AddMenuEntry(
        "PackTextures",
        LOCTEXT("PackTexturesMenuEntry", "Texture Channel Packer"),
        LOCTEXT("PackTexturesMenuEntryTooltip", "Opens the Texture Channel Packer tool."),
        FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Layout"),
        FUIAction(
            FExecuteAction::CreateLambda([]()
            {
                FGlobalTabmanager::Get()->TryInvokeTab(TextureChannelPackerTabName);
            })
        )
    );
}

void FTextureChannelPackerModule::ShutdownModule()
{
    // This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
    // we call this function before unloading the module.
    UToolMenus::UnregisterOwner(this);
    FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(TextureChannelPackerTabName);
}

TSharedRef<SDockTab> FTextureChannelPackerModule::OnSpawnPluginTab(const FSpawnTabArgs& SpawnTabArgs)
{
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

    return SNew(SDockTab)
        .TabRole(ETabRole::NomadTab)
        [
            SNew(SVerticalBox)

            // Red Channel Input
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(10.0f)
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot()
                .AutoHeight()
                .Padding(0.0f, 0.0f, 0.0f, 4.0f)
                [
                    SNew(STextBlock)
                    .Text(GetLocalizedMessage(TEXT("RedChannelLabel"), TEXT("Red Channel Input (e.g. Ambient Occlusion)"), TEXT("Red Channel Input (例: アンビエントオクルージョン)")))
                    .Font(FAppStyle::GetFontStyle("PropertyWindow.NormalFont"))
                ]
                + SVerticalBox::Slot()
                .AutoHeight()
                [
                    SNew(SObjectPropertyEntryBox)
                    .AllowedClass(UTexture2D::StaticClass())
                    .ObjectPath_Lambda([this]()
                    {
                        return InputTextureR.IsValid() ? InputTextureR->GetPathName() : FString();
                    })
                    .OnObjectChanged_Lambda([this](const FAssetData& AssetData)
                    {
                        InputTextureR = Cast<UTexture2D>(AssetData.GetAsset());
                        AutoGenerateFileName();
                    })
                    .AllowClear(true)
                    .DisplayThumbnail(true)
                ]
            ]

            // Green Channel Input
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(10.0f)
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot()
                .AutoHeight()
                .Padding(0.0f, 0.0f, 0.0f, 4.0f)
                [
                    SNew(STextBlock)
                    .Text(GetLocalizedMessage(TEXT("GreenChannelLabel"), TEXT("Green Channel Input (e.g. Roughness)"), TEXT("Green Channel Input (例: ラフネス)")))
                    .Font(FAppStyle::GetFontStyle("PropertyWindow.NormalFont"))
                ]
                + SVerticalBox::Slot()
                .AutoHeight()
                [
                    SNew(SObjectPropertyEntryBox)
                    .AllowedClass(UTexture2D::StaticClass())
                    .ObjectPath_Lambda([this]()
                    {
                        return InputTextureG.IsValid() ? InputTextureG->GetPathName() : FString();
                    })
                    .OnObjectChanged_Lambda([this](const FAssetData& AssetData)
                    {
                        InputTextureG = Cast<UTexture2D>(AssetData.GetAsset());
                        AutoGenerateFileName();
                    })
                    .AllowClear(true)
                    .DisplayThumbnail(true)
                ]
            ]

            // Blue Channel Input
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(10.0f)
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot()
                .AutoHeight()
                .Padding(0.0f, 0.0f, 0.0f, 4.0f)
                [
                    SNew(STextBlock)
                    .Text(GetLocalizedMessage(TEXT("BlueChannelLabel"), TEXT("Blue Channel Input (e.g. Metallic)"), TEXT("Blue Channel Input (例: メタリック)")))
                    .Font(FAppStyle::GetFontStyle("PropertyWindow.NormalFont"))
                ]
                + SVerticalBox::Slot()
                .AutoHeight()
                [
                    SNew(SObjectPropertyEntryBox)
                    .AllowedClass(UTexture2D::StaticClass())
                    .ObjectPath_Lambda([this]()
                    {
                        return InputTextureB.IsValid() ? InputTextureB->GetPathName() : FString();
                    })
                    .OnObjectChanged_Lambda([this](const FAssetData& AssetData)
                    {
                        InputTextureB = Cast<UTexture2D>(AssetData.GetAsset());
                        AutoGenerateFileName();
                    })
                    .AllowClear(true)
                    .DisplayThumbnail(true)
                ]
            ]

            // Alpha Channel Input
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(10.0f)
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot()
                .AutoHeight()
                .Padding(0.0f, 0.0f, 0.0f, 4.0f)
                [
                    SNew(STextBlock)
                    .Text(GetLocalizedMessage(TEXT("AlphaChannelLabel"), TEXT("Alpha Channel Input (Optional)"), TEXT("Alpha Channel Input (任意)")))
                    .Font(FAppStyle::GetFontStyle("PropertyWindow.NormalFont"))
                ]
                + SVerticalBox::Slot()
                .AutoHeight()
                [
                    SNew(SObjectPropertyEntryBox)
                    .AllowedClass(UTexture2D::StaticClass())
                    .ObjectPath_Lambda([this]()
                    {
                        return InputTextureA.IsValid() ? InputTextureA->GetPathName() : FString();
                    })
                    .OnObjectChanged_Lambda([this](const FAssetData& AssetData)
                    {
                        InputTextureA = Cast<UTexture2D>(AssetData.GetAsset());
                        AutoGenerateFileName();
                    })
                    .AllowClear(true)
                    .DisplayThumbnail(true)
                ]
            ]

            // Separator
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(10.0f, 5.0f)
            [
                SNew(SSeparator)
            ]

            // Output Settings Header
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(10.0f, 5.0f)
            [
                SNew(STextBlock)
                .Text(LOCTEXT("OutputSettingsLabel", "Output Settings"))
                .Font(FAppStyle::GetFontStyle("PropertyWindow.BoldFont"))
            ]

            // Resolution
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(10.0f, 5.0f)
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot()
                .AutoHeight()
                .Padding(0.0f, 0.0f, 0.0f, 4.0f)
                [
                    SNew(STextBlock)
                    .Text(LOCTEXT("ResolutionLabel", "Resolution (e.g. 2048)"))
                    .ToolTipText(GetLocalizedMessage(TEXT("ResolutionTooltip"), TEXT("Valid range: 1 - 8192"), TEXT("有効範囲: 1 - 8192")))
                    .Font(FAppStyle::GetFontStyle("PropertyWindow.NormalFont"))
                ]
                + SVerticalBox::Slot()
                .AutoHeight()
                [
                    SNew(SNumericEntryBox<int32>)
                    .Value_Lambda([this] { return TargetResolution; })
                    .OnValueChanged_Lambda([this](int32 NewValue) { TargetResolution = NewValue; })
                    .AllowSpin(true)
                    .MinValue(1)
                    .MaxValue(8192)
                    .MinSliderValue(1)
                    .MaxSliderValue(8192)
                ]
            ]

            // Compression Settings
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(10.0f, 5.0f)
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot()
                .AutoHeight()
                .Padding(0.0f, 0.0f, 0.0f, 4.0f)
                [
                    SNew(STextBlock)
                    .Text(LOCTEXT("CompressionLabel", "Compression"))
                    .Font(FAppStyle::GetFontStyle("PropertyWindow.NormalFont"))
                ]
                + SVerticalBox::Slot()
                .AutoHeight()
                [
                    SNew(SComboBox<TSharedPtr<FString>>)
                    .ToolTipText(GetLocalizedMessage(
                        TEXT("CompressionTooltip"),
                        TEXT("Select the compression method for the output texture.\n- Masks: Best for ORM (Occlusion, Roughness, Metallic) or other packed data. (Linear, no sRGB)\n- Grayscale: Best for single-channel values like Height or Alpha masks. (Linear)\n- Default: Standard compression. Not recommended for packed masks."),
                        TEXT("出力テクスチャの圧縮方式を選択します。\n- Masks: ORM (Occlusion, Roughness, Metallic) やパック済みデータに最適 (リニア, sRGBなし)\n- Grayscale: ハイトマップや単一マスクなど1チャンネルの値に最適 (リニア)\n- Default: 標準圧縮。パック済みマスクには非推奨")
                    ))
                    .OptionsSource(&CompressionOptions)
                    .OnSelectionChanged_Lambda([this](TSharedPtr<FString> NewSelection, ESelectInfo::Type)
                    {
                        if (NewSelection.IsValid())
                        {
                            CurrentCompressionOption = NewSelection;
                        }
                    })
                    .OnGenerateWidget_Lambda([](TSharedPtr<FString> Item)
                    {
                        return SNew(STextBlock).Text(FText::FromString(*Item));
                    })
                    [
                        SNew(STextBlock)
                        .Text_Lambda([this]()
                        {
                            return CurrentCompressionOption.IsValid() ? FText::FromString(*CurrentCompressionOption) : FText::GetEmpty();
                        })
                    ]
                ]
            ]

            // Output Path
            + SVerticalBox::Slot()
            .AutoHeight()
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

            // File Name
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(10.0f, 5.0f)
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot()
                .AutoHeight()
                .Padding(0.0f, 0.0f, 0.0f, 4.0f)
                [
                    SNew(STextBlock)
                    .Text(LOCTEXT("FileNameLabel", "File Name"))
                    .Font(FAppStyle::GetFontStyle("PropertyWindow.NormalFont"))
                ]
                + SVerticalBox::Slot()
                .AutoHeight()
                [
                    SNew(SEditableTextBox)
                    .Text_Lambda([this] { return FText::FromString(OutputFileName); })
                    .OnTextCommitted_Lambda([this](const FText& NewText, ETextCommit::Type) { OutputFileName = NewText.ToString(); })
                ]
            ]

            // Warning Text
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(20.0f, 0.0f, 20.0f, 0.0f)
            .HAlign(HAlign_Center)
            [
                SNew(STextBlock)
                .Text(GetLocalizedMessage(TEXT("HighPrecisionWarning"), TEXT("High-precision input detected. Output will use 16-bit float format."), TEXT("高精度入力が検出されました。出力は16bit Float形式になります。")))
                .ColorAndOpacity(FLinearColor(1.0f, 1.0f, 0.0f)) // Yellow
                .Visibility_Lambda([this]()
                {
                    return IsHighPrecisionInput() ? EVisibility::Visible : EVisibility::Collapsed;
                })
            ]

            // Spacer
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(10.0f)
            [
                SNew(SSpacer)
                .Size(FVector2D(0.0f, 10.0f))
            ]

            // Generate Button
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
                    return OnGenerateClicked();
                })
                [
                    SNew(STextBlock)
                    .Text(LOCTEXT("GenerateButtonText", "Generate Texture"))
                    .Font(FAppStyle::GetFontStyle("PropertyWindow.BoldFont"))
                ]
            ]
        ];
}


FReply FTextureChannelPackerModule::OnGenerateClicked()
{
    UE_LOG(LogTexturePacker, Log, TEXT("Input Format Detection:"));
    UE_LOG(LogTexturePacker, Log, TEXT("  Red: %s"), InputTextureR.IsValid() ? *UEnum::GetValueAsString(InputTextureR->Source.GetFormat()) : TEXT("None"));
    UE_LOG(LogTexturePacker, Log, TEXT("  Green: %s"), InputTextureG.IsValid() ? *UEnum::GetValueAsString(InputTextureG->Source.GetFormat()) : TEXT("None"));
    UE_LOG(LogTexturePacker, Log, TEXT("  Blue: %s"), InputTextureB.IsValid() ? *UEnum::GetValueAsString(InputTextureB->Source.GetFormat()) : TEXT("None"));
    UE_LOG(LogTexturePacker, Log, TEXT("  Alpha: %s"), InputTextureA.IsValid() ? *UEnum::GetValueAsString(InputTextureA->Source.GetFormat()) : TEXT("None"));
    UE_LOG(LogTexturePacker, Log, TEXT("Selected Output Format: %s"), *UEnum::GetValueAsString(DetermineOutputFormat()));

    UE_LOG(LogTexturePacker, Log, TEXT("Generating Texture..."));
    UE_LOG(LogTexturePacker, Log, TEXT("Resolution: %d"), TargetResolution);
    UE_LOG(LogTexturePacker, Log, TEXT("Output Path: %s"), *OutputPackagePath);
    UE_LOG(LogTexturePacker, Log, TEXT("File Name: %s"), *OutputFileName);

    // Validation Check 1: At least one input texture
    if (!InputTextureR.IsValid() && !InputTextureG.IsValid() && !InputTextureB.IsValid() && !InputTextureA.IsValid())
    {
        FText Msg = GetLocalizedMessage(TEXT("ErrorNoTextures"), TEXT("Please select at least one input texture."), TEXT("入力テクスチャを少なくとも1つ選択してください。"));
        ShowNotification(Msg, false);
        return FReply::Handled();
    }

    // Validation Check 2: Output filename is not empty
    if (OutputFileName.IsEmpty())
    {
        FText Msg = GetLocalizedMessage(TEXT("ErrorNoFileName"), TEXT("Please specify a file name."), TEXT("ファイル名を指定してください。"));
        ShowNotification(Msg, false);
        return FReply::Handled();
    }

    // Validation Check 3: Resolution is valid
    if (TargetResolution < 1 || TargetResolution > 8192)
    {
        FText Msg = GetLocalizedMessage(TEXT("ErrorInvalidResolution"), TEXT("Resolution must be between 1 and 8192."), TEXT("解像度は 1 から 8192 の間で指定してください。"));
        ShowNotification(Msg, false);
        return FReply::Handled();
    }

    FString PackageName = OutputPackagePath;
    if (!PackageName.EndsWith(TEXT("/")))
    {
        PackageName += TEXT("/");
    }
    PackageName += OutputFileName;

    CreateTexture(PackageName, TargetResolution);

    return FReply::Handled();
}

void FTextureChannelPackerModule::AutoGenerateFileName()
{
    TArray<FString> InputNames;
    if (InputTextureR.IsValid()) InputNames.Add(InputTextureR->GetName());
    if (InputTextureG.IsValid()) InputNames.Add(InputTextureG->GetName());
    if (InputTextureB.IsValid()) InputNames.Add(InputTextureB->GetName());
    if (InputTextureA.IsValid()) InputNames.Add(InputTextureA->GetName());

    if (InputNames.Num() == 0)
    {
        return;
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

    // Remove trailing underscores
    while (BaseName.EndsWith(TEXT("_")))
    {
        BaseName.LeftChopInline(1);
    }

    OutputFileName = BaseName + TEXT("_ORM");
}

// Helper function to read and resize texture data
static void GetResizedTextureData(UTexture2D* SourceTex, int32 TargetSize, bool bHighPrecision, TArray<uint8>& OutData8, TArray<FFloat16>& OutData16)
{
    OutData8.Empty();
    OutData16.Empty();

    if (!SourceTex)
    {
        if (bHighPrecision) OutData16.Init(FFloat16(0.0f), TargetSize * TargetSize * 4);
        else OutData8.Init(0, TargetSize * TargetSize * 4);
        return;
    }

#if WITH_EDITORONLY_DATA
    int32 SrcWidth = SourceTex->Source.GetSizeX();
    int32 SrcHeight = SourceTex->Source.GetSizeY();
    ETextureSourceFormat SrcFormat = SourceTex->Source.GetFormat();
    uint8* SrcData = SourceTex->Source.LockMip(0);

    if (!SrcData)
    {
        UE_LOG(LogTexturePacker, Warning, TEXT("Failed to lock source mip for texture: %s"), *SourceTex->GetName());
        if (bHighPrecision) OutData16.Init(FFloat16(0.0f), TargetSize * TargetSize * 4);
        else OutData8.Init(0, TargetSize * TargetSize * 4);
        return;
    }

    int32 NumPixels = SrcWidth * SrcHeight;

    if (bHighPrecision)
    {
        TArray<FLinearColor> SrcColors;
        SrcColors.SetNumUninitialized(NumPixels);

        // Convert to FLinearColor
        switch(SrcFormat)
        {
            case TSF_BGRA8:
            {
                 const FColor* Colors = (const FColor*)SrcData;
                 for(int32 i=0; i<NumPixels; ++i) SrcColors[i] = FLinearColor(Colors[i]);
                 break;
            }
            case TSF_G8:
            {
                for(int32 i=0; i<NumPixels; ++i)
                {
                    float V = (float)SrcData[i] / 255.0f;
                    SrcColors[i] = FLinearColor(V, V, V, 1.0f);
                }
                break;
            }
            case TSF_G16:
            {
                const uint16* D = (const uint16*)SrcData;
                for(int32 i=0; i<NumPixels; ++i)
                {
                    float V = (float)D[i] / 65535.0f;
                    SrcColors[i] = FLinearColor(V, V, V, 1.0f);
                }
                break;
            }
            case TSF_R16F:
            {
                const FFloat16* D = (const FFloat16*)SrcData;
                for(int32 i=0; i<NumPixels; ++i)
                {
                    float V = (float)D[i];
                    SrcColors[i] = FLinearColor(V, V, V, 1.0f);
                }
                break;
            }
            case TSF_R32F:
            {
                const float* D = (const float*)SrcData;
                for(int32 i=0; i<NumPixels; ++i)
                {
                    float V = D[i];
                    SrcColors[i] = FLinearColor(V, V, V, 1.0f);
                }
                break;
            }
            case TSF_RGBA32F:
            {
                 const FLinearColor* D = (const FLinearColor*)SrcData;
                 FMemory::Memcpy(SrcColors.GetData(), D, NumPixels * sizeof(FLinearColor));
                 break;
            }
            default:
            {
                 UE_LOG(LogTexturePacker, Warning, TEXT("Unsupported Source Format for High Precision: %d"), (int32)SrcFormat);
                 for(int32 i=0; i<NumPixels; ++i) SrcColors[i] = FLinearColor::Black;
                 break;
            }
        }

        SourceTex->Source.UnlockMip(0);

        TArray<FLinearColor> ResizedColors;
        if (SrcWidth != TargetSize || SrcHeight != TargetSize)
        {
            ResizedColors.SetNum(TargetSize * TargetSize);
            // Linear space interpolation is appropriate for data textures
            FImageUtils::ImageResize(SrcWidth, SrcHeight, SrcColors, TargetSize, TargetSize, ResizedColors, true);
        }
        else
        {
            ResizedColors = SrcColors;
        }

        OutData16.SetNumUninitialized(TargetSize * TargetSize * 4);
        for(int32 i=0; i<TargetSize * TargetSize; ++i)
        {
            const FLinearColor& C = ResizedColors[i];
            OutData16[i*4 + 0] = FFloat16(C.R);
            OutData16[i*4 + 1] = FFloat16(C.G);
            OutData16[i*4 + 2] = FFloat16(C.B);
            OutData16[i*4 + 3] = FFloat16(C.A);
        }
    }
    else
    {
        // Legacy 8-bit path (Preserved from original code)
        TArray<FColor> SrcColors;
        SrcColors.SetNumUninitialized(NumPixels);

        // Convert input to FColor (BGRA)
        switch (SrcFormat)
        {
        case TSF_BGRA8:
        {
            FMemory::Memcpy(SrcColors.GetData(), SrcData, NumPixels * sizeof(FColor));
            break;
        }
        case TSF_G8:
        {
            const uint8* GrayData = SrcData;
            for (int32 i = 0; i < NumPixels; ++i)
            {
                uint8 Val = GrayData[i];
                SrcColors[i] = FColor(Val, Val, Val, 255);
            }
            break;
        }
        case TSF_G16:
        {
            const uint16* GrayData16 = (const uint16*)SrcData;
            for (int32 i = 0; i < NumPixels; ++i)
            {
                uint8 Val = (uint8)(GrayData16[i] >> 8);
                SrcColors[i] = FColor(Val, Val, Val, 255);
            }
            break;
        }
        case TSF_R16F:
        {
            const FFloat16* Pixel16 = (const FFloat16*)SrcData;
            for (int32 i = 0; i < NumPixels; ++i)
            {
                uint8 Val = (uint8)FMath::Clamp<float>((float)Pixel16[i] * 255.0f, 0.0f, 255.0f);
                SrcColors[i] = FColor(Val, Val, Val, 255);
            }
            break;
        }
        case TSF_R32F:
        {
            const float* Pixel32 = (const float*)SrcData;
            for (int32 i = 0; i < NumPixels; ++i)
            {
                uint8 Val = (uint8)FMath::Clamp<float>(Pixel32[i] * 255.0f, 0.0f, 255.0f);
                SrcColors[i] = FColor(Val, Val, Val, 255);
            }
            break;
        }
        case TSF_RGBA32F:
        {
            const FLinearColor* LinearColors = (const FLinearColor*)SrcData;
            for (int32 i = 0; i < NumPixels; ++i)
            {
                uint8 Val = (uint8)FMath::Clamp<float>(LinearColors[i].R * 255.0f, 0.0f, 255.0f);
                SrcColors[i] = FColor(Val, Val, Val, 255);
            }
            break;
        }
        default:
        {
            UE_LOG(LogTexturePacker, Warning, TEXT("Unsupported Source Format: %d for texture: %s"), (int32)SrcFormat, *SourceTex->GetName());
            FMemory::Memset(SrcColors.GetData(), 0, NumPixels * sizeof(FColor));
            break;
        }
        }

        SourceTex->Source.UnlockMip(0);

        // Resize if necessary
        TArray<FColor> ResizedColors;
        if (SrcWidth != TargetSize || SrcHeight != TargetSize)
        {
            ResizedColors.SetNum(TargetSize * TargetSize);
            FImageUtils::ImageResize(SrcWidth, SrcHeight, SrcColors, TargetSize, TargetSize, ResizedColors, false);
        }
        else
        {
            ResizedColors = SrcColors;
        }

        // Convert FColor (BGRA) to RGBA uint8 array
        OutData8.SetNumUninitialized(TargetSize * TargetSize * 4);
        uint8* DestData = OutData8.GetData();
        for (int32 i = 0; i < TargetSize * TargetSize; ++i)
        {
            const FColor& C = ResizedColors[i];
            DestData[i * 4 + 0] = C.R;
            DestData[i * 4 + 1] = C.G;
            DestData[i * 4 + 2] = C.B;
            DestData[i * 4 + 3] = C.A;
        }
    }
#else
    UE_LOG(LogTexturePacker, Error, TEXT("TextureChannelPacker requires WITH_EDITORONLY_DATA to access Source."));
    if (bHighPrecision) OutData16.Init(FFloat16(0.0f), TargetSize * TargetSize * 4);
    else OutData8.Init(0, TargetSize * TargetSize * 4);
#endif
}

void FTextureChannelPackerModule::CreateTexture(const FString& PackageName, int32 Resolution)
{
    check(IsInGameThread());

    ETextureSourceFormat OutputFormat = DetermineOutputFormat();
    bool bHighPrecision = (OutputFormat == TSF_RGBA16F);

    // Create the package
    UPackage* Package = CreatePackage(*PackageName);
    Package->FullyLoad();

    // Create the Texture2D
    FName TextureName = FName(*FPaths::GetBaseFilename(PackageName));
    UTexture2D* NewTexture = NewObject<UTexture2D>(Package, TextureName, RF_Public | RF_Standalone | RF_MarkAsRootSet);

    // Get Resized Data for Inputs
    TArray<uint8> DataR_8, DataG_8, DataB_8, DataA_8;
    TArray<FFloat16> DataR_16, DataG_16, DataB_16, DataA_16;

    GetResizedTextureData(InputTextureR.Get(), Resolution, bHighPrecision, DataR_8, DataR_16);
    GetResizedTextureData(InputTextureG.Get(), Resolution, bHighPrecision, DataG_8, DataG_16);
    GetResizedTextureData(InputTextureB.Get(), Resolution, bHighPrecision, DataB_8, DataB_16);
    GetResizedTextureData(InputTextureA.Get(), Resolution, bHighPrecision, DataA_8, DataA_16);

    bool bHasAlpha = InputTextureA.IsValid();

#if WITH_EDITORONLY_DATA
    // Initialize Source
    NewTexture->Source.Init(Resolution, Resolution, 1, 1, OutputFormat);

    // Lock and Write Pixels directly to Source
    uint8* MipData = NewTexture->Source.LockMip(0);
    if (MipData)
    {
        if (bHighPrecision)
        {
             FFloat16* MipData16 = (FFloat16*)MipData;
             for (int32 i = 0; i < Resolution * Resolution; ++i)
             {
                 // DataX_16 is RGBA. Use Red channel (index 0).
                 FFloat16 R_Val = DataR_16[i * 4 + 0];
                 FFloat16 G_Val = DataG_16[i * 4 + 0];
                 FFloat16 B_Val = DataB_16[i * 4 + 0];
                 FFloat16 A_Val = bHasAlpha ? DataA_16[i * 4 + 0] : FFloat16(1.0f);

                 // Output Texture TSF_RGBA16F is RGBA
                 MipData16[i * 4 + 0] = R_Val;
                 MipData16[i * 4 + 1] = G_Val;
                 MipData16[i * 4 + 2] = B_Val;
                 MipData16[i * 4 + 3] = A_Val;
             }
        }
        else
        {
            for (int32 i = 0; i < Resolution * Resolution; ++i)
            {
                // DataX_8 is RGBA. Use Red channel (index 0).
                uint8 R_Val = DataR_8[i * 4 + 0];
                uint8 G_Val = DataG_8[i * 4 + 0];
                uint8 B_Val = DataB_8[i * 4 + 0];
                uint8 A_Val = bHasAlpha ? DataA_8[i * 4 + 0] : 255;

                // Output Texture TSF_BGRA8 is BGRA
                MipData[i * 4 + 0] = B_Val; // B
                MipData[i * 4 + 1] = G_Val; // G
                MipData[i * 4 + 2] = R_Val; // R
                MipData[i * 4 + 3] = A_Val; // A
            }
        }
    }
    NewTexture->Source.UnlockMip(0);
#endif

    // Final settings
    NewTexture->CompressionSettings = GetSelectedCompressionSettings();
    NewTexture->SRGB = false;
    NewTexture->UpdateResource();

    Package->MarkPackageDirty();
    FAssetRegistryModule::AssetCreated(NewTexture);

    FText FormatPattern = GetLocalizedMessage(TEXT("SuccessTextureSaved"), TEXT("Texture Saved: {0}"), TEXT("テクスチャを保存しました: {0}"));
    FText FinalMessage = FText::Format(FormatPattern, FText::FromString(PackageName));

    if (bHighPrecision)
    {
        FinalMessage = FText::Format(FText::FromString(TEXT("{0} (16-bit Float)")), FinalMessage);
    }

    ShowNotification(FinalMessage, true);
}

void FTextureChannelPackerModule::ShowNotification(const FText& Message, bool bSuccess)
{
    FNotificationInfo Info(Message);
    Info.ExpireDuration = 3.0f;

    if (bSuccess)
    {
        Info.Image = FAppStyle::GetBrush("Icons.SuccessWithColor");
    }
    else
    {
        Info.Image = FAppStyle::GetBrush("Icons.ErrorWithColor");
    }

    TSharedPtr<SNotificationItem> NotificationItem = FSlateNotificationManager::Get().AddNotification(Info);
    if (NotificationItem.IsValid())
    {
        NotificationItem->SetCompletionState(bSuccess ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
        NotificationItem->ExpireAndFadeout();
    }
}

TextureCompressionSettings FTextureChannelPackerModule::GetSelectedCompressionSettings() const
{
    if (CurrentCompressionOption.IsValid())
    {
        const FString& Option = *CurrentCompressionOption;
        if (Option == "Masks (Recommended)") return TC_Masks;
        if (Option == "Grayscale") return TC_Grayscale;
        if (Option == "Default") return TC_Default;
    }
    return TC_Masks; // Fallback
}

bool FTextureChannelPackerModule::IsHighPrecisionInput() const
{
    auto CheckTexture = [](TWeakObjectPtr<UTexture2D> Tex) -> bool
    {
        if (Tex.IsValid())
        {
            ETextureSourceFormat Fmt = Tex->Source.GetFormat();
            return (Fmt == TSF_G16 || Fmt == TSF_R16F || Fmt == TSF_R32F || Fmt == TSF_RGBA32F);
        }
        return false;
    };

    return CheckTexture(InputTextureR) || CheckTexture(InputTextureG) || CheckTexture(InputTextureB) || CheckTexture(InputTextureA);
}

ETextureSourceFormat FTextureChannelPackerModule::DetermineOutputFormat() const
{
    return IsHighPrecisionInput() ? TSF_RGBA16F : TSF_BGRA8;
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FTextureChannelPackerModule, TextureChannelPacker)
