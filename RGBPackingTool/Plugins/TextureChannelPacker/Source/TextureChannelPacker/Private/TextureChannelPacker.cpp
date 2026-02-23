#include "TextureChannelPacker.h"
#include "UObject/StrongObjectPtr.h"
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
#include "Misc/MessageDialog.h"
#include "ImageUtils.h"
#include "Math/UnrealMathUtility.h"
#include "Math/Float16.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Images/SImage.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "ThumbnailRendering/ThumbnailManager.h"
#include "Internationalization/Internationalization.h"
#include "Internationalization/Culture.h"
#include "Misc/ScopedSlowTask.h"
#include "Async/ParallelFor.h"

#define LOCTEXT_NAMESPACE "FTextureChannelPackerModule"

DEFINE_LOG_CATEGORY_STATIC(LogTexturePacker, Log, All);

static const FName TextureChannelPackerTabName("TextureChannelPacker");

/**
 * @brief Retrieves a localized message based on the current culture.
 *
 * This helper function returns either the Japanese text (if the current culture is Japanese)
 * or the English text (for all other cultures).
 *
 * @param Key A unique identifier for the localization key (currently unused but good for future expansion).
 * @param EnglishText The text to display in English.
 * @param JapaneseText The text to display in Japanese.
 * @return FText The localized text.
 */
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

TSharedRef<SWidget> FTextureChannelPackerModule::CreateChannelInputSlot(const FText& LabelText, TWeakObjectPtr<UTexture2D>& TargetTexturePtr, const FText& TooltipText)
{
    // Capture the address of the member variable to update it inside the lambda
    TWeakObjectPtr<UTexture2D>* TexturePtr = &TargetTexturePtr;

    TSharedPtr<STextBlock> LabelWidget = SNew(STextBlock)
        .Text(LabelText)
        .Font(FAppStyle::GetFontStyle("PropertyWindow.NormalFont"));

    if (!TooltipText.IsEmpty())
    {
        LabelWidget->SetToolTipText(TooltipText);
    }

    return SNew(SVerticalBox)
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(0.0f, 0.0f, 0.0f, 4.0f)
        [
            LabelWidget.ToSharedRef()
        ]
        + SVerticalBox::Slot()
        .AutoHeight()
        [
            SNew(SObjectPropertyEntryBox)
            .AllowedClass(UTexture2D::StaticClass())
            .ObjectPath_Lambda([TexturePtr]()
            {
                return TexturePtr->IsValid() ? TexturePtr->Get()->GetPathName() : FString();
            })
            .OnObjectChanged_Lambda([this, TexturePtr](const FAssetData& AssetData)
            {
                *TexturePtr = Cast<UTexture2D>(AssetData.GetAsset());
                AutoGenerateFileName();
            })
            .AllowClear(true)
            .DisplayThumbnail(true)
            .ThumbnailPool(UThumbnailManager::Get().GetSharedThumbnailPool())
        ];
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
                CreateChannelInputSlot(
                    GetLocalizedMessage(TEXT("RedChannelLabel"), TEXT("Red Channel Input (e.g. Ambient Occlusion)"), TEXT("Red Channel Input (例: アンビエントオクルージョン)")),
                    InputTextureR
                )
            ]

            // Green Channel Input
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(10.0f)
            [
                CreateChannelInputSlot(
                    GetLocalizedMessage(TEXT("GreenChannelLabel"), TEXT("Green Channel Input (e.g. Roughness)"), TEXT("Green Channel Input (例: ラフネス)")),
                    InputTextureG
                )
            ]

            // Blue Channel Input
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(10.0f)
            [
                CreateChannelInputSlot(
                    GetLocalizedMessage(TEXT("BlueChannelLabel"), TEXT("Blue Channel Input (e.g. Metallic)"), TEXT("Blue Channel Input (例: メタリック)")),
                    InputTextureB
                )
            ]

            // Alpha Channel Input (with Tooltip)
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(10.0f)
            [
                CreateChannelInputSlot(
                    GetLocalizedMessage(TEXT("AlphaChannelLabel"), TEXT("Alpha Channel Input (Optional)"), TEXT("Alpha Channel Input (任意)")),
                    InputTextureA,
                    GetLocalizedMessage(
                        TEXT("AlphaChannelTooltip"),
                        TEXT("If left empty, fills with White (255) to ensure opacity. Assign a texture to pack a custom Alpha mask."),
                        TEXT("空の場合は白 (255) で塗りつぶされ、不透明になります。独自のアルファマスクを使用する場合はテクスチャを指定してください。")
                    )
                )
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
                    .OnTextCommitted_Lambda([this](const FText& NewText, ETextCommit::Type CommitType)
                    {
                        OutputFileName = NewText.ToString();
                        if (CommitType == ETextCommit::OnEnter || CommitType == ETextCommit::OnUserMovedFocus)
                        {
                            bFileNameManuallyEdited = true;
                        }
                    })
                ]
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
    UE_LOG(LogTexturePacker, Log, TEXT("Generating Texture..."));
    UE_LOG(LogTexturePacker, Log, TEXT("Input Red: %s"), InputTextureR.IsValid() ? *InputTextureR->GetPathName() : TEXT("None"));
    UE_LOG(LogTexturePacker, Log, TEXT("Input Green: %s"), InputTextureG.IsValid() ? *InputTextureG->GetPathName() : TEXT("None"));
    UE_LOG(LogTexturePacker, Log, TEXT("Input Blue: %s"), InputTextureB.IsValid() ? *InputTextureB->GetPathName() : TEXT("None"));
    UE_LOG(LogTexturePacker, Log, TEXT("Input Alpha: %s"), InputTextureA.IsValid() ? *InputTextureA->GetPathName() : TEXT("None"));
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

    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
    FString ObjectPath = PackageName + TEXT(".") + OutputFileName;
    FAssetData ExistingAsset = AssetRegistryModule.Get().GetAssetByObjectPath(FSoftObjectPath(ObjectPath));

    if (ExistingAsset.IsValid())
    {
        FText Msg = FText::Format(
            GetLocalizedMessage(
                TEXT("ConfirmOverwrite"),
                TEXT("{0} already exists. Do you want to overwrite it?"),
                TEXT("{0} は既に存在します。上書きしますか？")
            ),
            FText::FromString(OutputFileName)
        );

        EAppReturnType::Type Result = FMessageDialog::Open(
            EAppMsgType::YesNo,
            Msg
        );

        if (Result == EAppReturnType::No)
        {
            return FReply::Handled();
        }
    }

    CreateTexture(PackageName, TargetResolution);

    return FReply::Handled();
}

void FTextureChannelPackerModule::AutoGenerateFileName()
{
    if (bFileNameManuallyEdited)
    {
        return;
    }

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

    OutputFileName = BaseName + TEXT("_ORM");
}

/**
 * @struct FTextureRawData
 * @brief Holds raw texture data extracted from a UTexture2D.
 *
 * This struct is used to transfer texture data from the Game Thread (where UTexture2D is accessible)
 * to background threads for processing. It ensures thread safety by copying necessary data
 * (dimensions, format, raw bytes) beforehand.
 */
struct FTextureRawData
{
    TArray<uint8> RawData;
    int32 Width = 0;
    int32 Height = 0;
    ETextureSourceFormat Format = TSF_Invalid;
    FString TextureName;
    bool bIsValid = false;

    /**
     * User-facing error message if extraction failed.
     * Empty if no error occurred.
     */
    FText ErrorMessage;
};

/**
 * @struct FTextureProcessResult
 * @brief Represents the result of a texture processing operation.
 *
 * This struct contains the processed pixel data for a specific channel or an error message
 * if the operation failed. It is generated by background threads and consumed by the Game Thread.
 */
struct FTextureProcessResult
{
    TArray<uint8> ProcessedData;
    FText ErrorMessage;
    bool bSuccess = true;
};

/**
 * @brief Extracts raw pixel data from a UTexture2D on the Game Thread.
 *
 * This function accesses the platform-specific source data of a texture asset,
 * locks the mipmap to read raw bytes, and copies them into a thread-safe struct.
 * This MUST be called on the Game Thread.
 *
 * @param SourceTex The source UTexture2D asset.
 * @return FTextureRawData A struct containing the copied raw data and metadata.
 */
static FTextureRawData ExtractTextureSourceData(UTexture2D* SourceTex)
{
    FTextureRawData Result;
    if (!SourceTex)
    {
        return Result;
    }

    Result.TextureName = SourceTex->GetName();

#if WITH_EDITORONLY_DATA
    Result.Width = SourceTex->Source.GetSizeX();
    Result.Height = SourceTex->Source.GetSizeY();
    Result.Format = SourceTex->Source.GetFormat();

    uint8* SrcData = SourceTex->Source.LockMip(0);
    if (SrcData)
    {
        int32 BytesPerPixel = SourceTex->Source.GetBytesPerPixel();

        // Validation 1: Check if BytesPerPixel is valid
        if (BytesPerPixel == 0)
        {
            UE_LOG(LogTexturePacker, Error,
                TEXT("GetBytesPerPixel() returned 0 for texture: %s (Format: %d). This format may not be supported."),
                *Result.TextureName, (int32)Result.Format);
            SourceTex->Source.UnlockMip(0);
            return Result;  // Return invalid result
        }

        int32 TotalBytes = Result.Width * Result.Height * BytesPerPixel;

        // Validation 2: Check if TotalBytes is valid
        if (TotalBytes <= 0)
        {
            UE_LOG(LogTexturePacker, Error,
                TEXT("Invalid total bytes (%d) for texture: %s (Width: %d, Height: %d, BPP: %d)"),
                TotalBytes, *Result.TextureName, Result.Width, Result.Height, BytesPerPixel);
            SourceTex->Source.UnlockMip(0);
            return Result;  // Return invalid result
        }

        // Data is valid, proceed with copy
        Result.RawData.SetNumUninitialized(TotalBytes);
        FMemory::Memcpy(Result.RawData.GetData(), SrcData, TotalBytes);
        Result.bIsValid = true;
    }
    else
    {
        UE_LOG(LogTexturePacker, Warning, TEXT("Failed to lock source mip for texture: %s"), *Result.TextureName);
        Result.ErrorMessage = GetLocalizedMessage(
            TEXT("ErrorLockFailed"),
            TEXT("Failed to access texture data. The texture may be corrupted or in use. Try reimporting the texture."),
            TEXT("テクスチャデータへのアクセスに失敗しました。テクスチャが破損しているか、使用中の可能性があります。テクスチャを再インポートしてください。")
        );
    }
    SourceTex->Source.UnlockMip(0);
#else
    UE_LOG(LogTexturePacker, Error, TEXT("TextureChannelPacker requires WITH_EDITORONLY_DATA to access Source."));
    Result.ErrorMessage = GetLocalizedMessage(
        TEXT("ErrorNoEditorData"),
        TEXT("This plugin requires Editor-only data to function. Ensure the project is built with editor support."),
        TEXT("このプラグインはエディター専用データが必要です。プロジェクトがエディターサポート付きでビルドされていることを確認してください。")
    );
#endif

    return Result;
}

/**
 * @brief Processes raw texture data to produce a single channel of output.
 *
 * This function handles resizing (using FImageUtils) and format conversion (e.g., extracting
 * the Red channel from BGRA, or converting 16-bit grayscale to 8-bit).
 * This function is designed to be thread-safe and run in parallel tasks.
 *
 * @param Input The raw source data extracted from the input texture.
 * @param TargetSize The target resolution for the output (width and height).
 * @return FTextureProcessResult The processed single-channel 8-bit data.
 */
static FTextureProcessResult ProcessTextureSourceData(FTextureRawData& Input, int32 TargetSize)
{
    FTextureProcessResult Result;
    // Default to zero-filled array
    Result.ProcessedData.Init(0, TargetSize * TargetSize);

    if (!Input.bIsValid)
    {
        return Result; // Empty/Invalid input results in black channel (or white if handled by caller default)
    }

    int32 SrcWidth = Input.Width;
    int32 SrcHeight = Input.Height;
    int32 NumPixels = SrcWidth * SrcHeight;
    const uint8* SrcData = Input.RawData.GetData();

    // Optimization: Fast path for same-resolution textures
    if (SrcWidth == TargetSize && SrcHeight == TargetSize)
    {
        if (Input.Format == TSF_G8)
        {
            // Direct move for Grayscale input (zero-copy optimization)
            Result.ProcessedData = MoveTemp(Input.RawData);
            return Result;
        }
        else if (Input.Format == TSF_BGRA8)
        {
            // Parallel Red-channel extraction for BGRA input
            Result.ProcessedData.SetNumUninitialized(NumPixels);
            uint8* DestData = Result.ProcessedData.GetData();
            const uint8* SrcPtr = SrcData;

            ParallelFor(NumPixels, [DestData, SrcPtr](int32 i)
            {
                DestData[i] = SrcPtr[i * 4 + 2]; // R channel in BGRA
            });
            return Result;
        }
    }

    TArray<FColor> SrcColors;
    SrcColors.SetNumUninitialized(NumPixels);

    // Convert input to FColor (BGRA)
    switch (Input.Format)
    {
    case TSF_BGRA8:
    {
        FMemory::Memcpy(SrcColors.GetData(), SrcData, Input.RawData.Num());
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
        // 16-bit Grayscale: 2 bytes per pixel
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
        // Half-float: 2 bytes per pixel
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
        // Float: 4 bytes per pixel
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
        // Linear Color: 16 bytes per pixel
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
        UE_LOG(LogTexturePacker, Error, TEXT("Unsupported Source Format: %d for texture: %s"), (int32)Input.Format, *Input.TextureName);
        Result.bSuccess = false;
        Result.ErrorMessage = GetLocalizedMessage(
            TEXT("ErrorUnsupportedFormat"),
            TEXT("Texture format not supported. Please convert to PNG or TGA."),
            TEXT("テクスチャ形式がサポートされていません。PNGまたはTGAに変換してください。")
        );
        return Result;
    }
    }

    // Resize if necessary
    TArray<FColor> ResizedColors;
    if (SrcWidth != TargetSize || SrcHeight != TargetSize)
    {
        ResizedColors.SetNum(TargetSize * TargetSize);
        FImageUtils::ImageResize(SrcWidth, SrcHeight, SrcColors, TargetSize, TargetSize, ResizedColors, false);
    }
    else
    {
        ResizedColors = MoveTemp(SrcColors);
    }

    // Convert FColor (BGRA) to uint8 array (Grayscale, 1 byte per pixel)
    Result.ProcessedData.SetNumUninitialized(TargetSize * TargetSize);
    uint8* DestData = Result.ProcessedData.GetData();
    for (int32 i = 0; i < TargetSize * TargetSize; ++i)
    {
        const FColor& C = ResizedColors[i];
        DestData[i] = C.R;
    }

    return Result;
}

void FTextureChannelPackerModule::CreateTexture(const FString& PackageName, int32 Resolution)
{
    check(IsInGameThread());

    // Initialize progress dialog with 6 steps total
    FScopedSlowTask SlowTask(6.0f, GetLocalizedMessage(
        TEXT("ProgressProcessing"),
        TEXT("Processing Textures..."),
        TEXT("テクスチャを処理中...")
    ));
    SlowTask.MakeDialog(true); // true = cancellable

    // Create the package using TStrongObjectPtr for RAII
    TStrongObjectPtr<UPackage> PackagePtr(CreatePackage(*PackageName));
    UPackage* Package = PackagePtr.Get();

    if (!Package)
    {
        ShowNotification(
            GetLocalizedMessage(
                TEXT("ErrorPackageCreation"),
                TEXT("Failed to create package."),
                TEXT("パッケージの作成に失敗しました。")
            ),
            false
        );
        return;
    }

    Package->FullyLoad();

    // Helper lambda for cleanup on early exit (Cancel/Error)
    auto CleanupOnEarlyExit = [&Package, &PackageName]()
    {
        if (Package && !Package->IsDirty())
        {
            UE_LOG(LogTexturePacker, Warning, TEXT("Package creation cancelled. Cleaning up: %s"), *PackageName);
            Package->ClearFlags(RF_Standalone | RF_MarkAsRootSet);
            Package->MarkAsGarbage();
        }
    };

    SlowTask.EnterProgressFrame(1.0f, GetLocalizedMessage(
        TEXT("ProgressPackageCreated"),
        TEXT("Package created. Loading input textures..."),
        TEXT("パッケージを作成しました。入力テクスチャを読み込み中...")
    ));

    if (SlowTask.ShouldCancel())
    {
        CleanupOnEarlyExit();
        FText CancelMsg = GetLocalizedMessage(
            TEXT("OperationCancelled"),
            TEXT("Texture generation was cancelled by user."),
            TEXT("テクスチャ生成がユーザーによってキャンセルされました。")
        );
        ShowNotification(CancelMsg, false);
        return;
    }

    // Create the Texture2D
    FName TextureName = FName(*FPaths::GetBaseFilename(PackageName));
    UTexture2D* NewTexture = NewObject<UTexture2D>(Package, TextureName, RF_Public | RF_Standalone | RF_MarkAsRootSet);

    // ---------------------------------------------------------
    // STEP 1: Extract Raw Data from Inputs (Game Thread)
    // ---------------------------------------------------------
    SlowTask.EnterProgressFrame(1.0f, GetLocalizedMessage(
        TEXT("ProgressExtracting"),
        TEXT("Extracting source data..."),
        TEXT("ソースデータを抽出中...")
    ));

    if (SlowTask.ShouldCancel())
    {
        CleanupOnEarlyExit();
        FText CancelMsg = GetLocalizedMessage(
            TEXT("OperationCancelled"),
            TEXT("Texture generation was cancelled by user."),
            TEXT("テクスチャ生成がユーザーによってキャンセルされました。")
        );
        ShowNotification(CancelMsg, false);
        return;
    }

    TArray<FTextureRawData> RawInputs;
    RawInputs.SetNum(4); // R, G, B, A

    RawInputs[0] = ExtractTextureSourceData(InputTextureR.Get());
    RawInputs[1] = ExtractTextureSourceData(InputTextureG.Get());
    RawInputs[2] = ExtractTextureSourceData(InputTextureB.Get());
    RawInputs[3] = ExtractTextureSourceData(InputTextureA.Get());

    // ---------------------------------------------------------
    // STEP 2: Process Data in Parallel (Background Threads)
    // ---------------------------------------------------------
    SlowTask.EnterProgressFrame(2.0f, GetLocalizedMessage(
        TEXT("ProgressProcessingParallel"),
        TEXT("Resizing and processing channels..."),
        TEXT("チャンネルのリサイズと処理中...")
    ));

    if (SlowTask.ShouldCancel())
    {
        CleanupOnEarlyExit();
        FText CancelMsg = GetLocalizedMessage(
            TEXT("OperationCancelled"),
            TEXT("Texture generation was cancelled by user."),
            TEXT("テクスチャ生成がユーザーによってキャンセルされました。")
        );
        ShowNotification(CancelMsg, false);
        return;
    }

    TArray<FTextureProcessResult> ProcessedResults;
    ProcessedResults.SetNum(4);

    ParallelFor(4, [&](int32 Index)
    {
        ProcessedResults[Index] = ProcessTextureSourceData(RawInputs[Index], Resolution);
    });

    if (SlowTask.ShouldCancel())
    {
        CleanupOnEarlyExit();
        FText CancelMsg = GetLocalizedMessage(
            TEXT("OperationCancelled"),
            TEXT("Texture generation was cancelled by user."),
            TEXT("テクスチャ生成がユーザーによってキャンセルされました。")
        );
        ShowNotification(CancelMsg, false);
        return;
    }

    // Check for errors
    for (const auto& Res : ProcessedResults)
    {
        if (!Res.bSuccess && !Res.ErrorMessage.IsEmpty())
        {
            ShowNotification(Res.ErrorMessage, false);
            // We continue, treating it as black/default, but user is warned.
            // Alternatively, return here to abort.
        }
    }

    // Check for errors from texture extraction
    for (int32 i = 0; i < RawInputs.Num(); ++i)
    {
        if (!RawInputs[i].bIsValid && !RawInputs[i].ErrorMessage.IsEmpty())
        {
            ShowNotification(RawInputs[i].ErrorMessage, false);
            // Continue processing - the channel will be filled with default values
        }
    }

#if WITH_EDITORONLY_DATA
    // ---------------------------------------------------------
    // STEP 3: Write to Output Texture (Game Thread)
    // ---------------------------------------------------------
    // Initialize Source
    NewTexture->Source.Init(Resolution, Resolution, 1, 1, TSF_BGRA8);

    SlowTask.EnterProgressFrame(1.0f, GetLocalizedMessage(
        TEXT("ProgressWritingPixels"),
        TEXT("Writing pixel data..."),
        TEXT("ピクセルデータを書き込み中...")
    ));

    if (SlowTask.ShouldCancel())
    {
        CleanupOnEarlyExit();
        FText CancelMsg = GetLocalizedMessage(
            TEXT("OperationCancelled"),
            TEXT("Texture generation was cancelled by user."),
            TEXT("テクスチャ生成がユーザーによってキャンセルされました。")
        );
        ShowNotification(CancelMsg, false);
        return;
    }

    // Lock and Write Pixels directly to Source
    uint8* MipData = NewTexture->Source.LockMip(0);
    if (MipData)
    {
        const uint8* TempR = ProcessedResults[0].ProcessedData.Num() > 0 ? ProcessedResults[0].ProcessedData.GetData() : nullptr;
        const uint8* TempG = ProcessedResults[1].ProcessedData.Num() > 0 ? ProcessedResults[1].ProcessedData.GetData() : nullptr;
        const uint8* TempB = ProcessedResults[2].ProcessedData.Num() > 0 ? ProcessedResults[2].ProcessedData.GetData() : nullptr;
        const uint8* TempA = ProcessedResults[3].ProcessedData.Num() > 0 ? ProcessedResults[3].ProcessedData.GetData() : nullptr;

        // Pre-fill defaults for null channels to eliminate branches in the main loop
        TArray<uint8> DefaultR, DefaultG, DefaultB, DefaultA;

        const uint8* PtrR = TempR;
        const uint8* PtrG = TempG;
        const uint8* PtrB = TempB;
        const uint8* PtrA = TempA;

        if (!PtrR)
        {
            DefaultR.Init(0, Resolution * Resolution);
            PtrR = DefaultR.GetData();
        }
        if (!PtrG)
        {
            DefaultG.Init(0, Resolution * Resolution);
            PtrG = DefaultG.GetData();
        }
        if (!PtrB)
        {
            DefaultB.Init(0, Resolution * Resolution);
            PtrB = DefaultB.GetData();
        }
        if (!PtrA)
        {
            DefaultA.Init(255, Resolution * Resolution);
            PtrA = DefaultA.GetData();
        }

        // Parallel, branch-free pixel writing
        ParallelFor(Resolution * Resolution, [MipData, PtrR, PtrG, PtrB, PtrA](int32 i)
        {
            int32 Offset = i * 4;
            MipData[Offset + 0] = PtrB[i]; // B
            MipData[Offset + 1] = PtrG[i]; // G
            MipData[Offset + 2] = PtrR[i]; // R
            MipData[Offset + 3] = PtrA[i]; // A
        });
    }
    NewTexture->Source.UnlockMip(0);
#endif

    SlowTask.EnterProgressFrame(1.0f, GetLocalizedMessage(
        TEXT("ProgressFinalizing"),
        TEXT("Finalizing texture..."),
        TEXT("テクスチャを最終処理中...")
    ));

    if (SlowTask.ShouldCancel())
    {
        CleanupOnEarlyExit();
        FText CancelMsg = GetLocalizedMessage(
            TEXT("OperationCancelled"),
            TEXT("Texture generation was cancelled by user."),
            TEXT("テクスチャ生成がユーザーによってキャンセルされました。")
        );
        ShowNotification(CancelMsg, false);
        return;
    }

    // Final settings
    NewTexture->CompressionSettings = GetSelectedCompressionSettings();

    // Even if TC_Default is selected, treat it as linear (sRGB=false) for channel packing purposes.
    NewTexture->SRGB = false;

    NewTexture->UpdateResource();
    NewTexture->PostEditChange();

    Package->MarkPackageDirty();
    FAssetRegistryModule::AssetCreated(NewTexture);

    FText FormatPattern = GetLocalizedMessage(TEXT("SuccessTextureSaved"), TEXT("Texture Saved: {0}"), TEXT("テクスチャを保存しました: {0}"));
    ShowNotification(FText::Format(FormatPattern, FText::FromString(PackageName)), true);
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

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FTextureChannelPackerModule, TextureChannelPacker)
