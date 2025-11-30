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
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Images/SImage.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"

#define LOCTEXT_NAMESPACE "FTextureChannelPackerModule"

DEFINE_LOG_CATEGORY_STATIC(LogTexturePacker, Log, All);

static const FName TextureChannelPackerTabName("TextureChannelPacker");

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
                    .Text(LOCTEXT("RedChannelLabel", "Red Channel Input"))
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
                    .Text(LOCTEXT("GreenChannelLabel", "Green Channel Input"))
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
                    .Text(LOCTEXT("BlueChannelLabel", "Blue Channel Input"))
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
                    .Text(LOCTEXT("AlphaChannelLabel", "Alpha Channel Input"))
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
                    .Font(FAppStyle::GetFontStyle("PropertyWindow.NormalFont"))
                ]
                + SVerticalBox::Slot()
                .AutoHeight()
                [
                    SNew(SNumericEntryBox<int32>)
                    .Value_Lambda([this] { return TargetResolution; })
                    .OnValueChanged_Lambda([this](int32 NewValue) { TargetResolution = NewValue; })
                    .AllowSpin(true)
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
        ShowNotification(LOCTEXT("ErrorNoTextures", "Please select at least one input texture."), false);
        return FReply::Handled();
    }

    // Validation Check 2: Output filename is not empty
    if (OutputFileName.IsEmpty())
    {
        ShowNotification(LOCTEXT("ErrorNoFileName", "Please specify a file name."), false);
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

// Helper function to read and resize texture data
static TArray<uint8> GetResizedTextureData(UTexture2D* SourceTex, int32 TargetSize)
{
    TArray<uint8> ResultData;
    ResultData.Init(0, TargetSize * TargetSize * 4); // RGBA (4 bytes per pixel)

    if (!SourceTex)
    {
        return ResultData;
    }

#if WITH_EDITORONLY_DATA
    // Access Source Data
    int32 SrcWidth = SourceTex->Source.GetSizeX();
    int32 SrcHeight = SourceTex->Source.GetSizeY();
    ETextureSourceFormat SrcFormat = SourceTex->Source.GetFormat();

    // Lock Mip 0
    uint8* SrcData = SourceTex->Source.LockMip(0);
    if (!SrcData)
    {
        UE_LOG(LogTexturePacker, Warning, TEXT("Failed to lock source mip for texture: %s"), *SourceTex->GetName());
        return ResultData;
    }

    int32 NumPixels = SrcWidth * SrcHeight;
    TArray<FColor> SrcColors;
    SrcColors.SetNumUninitialized(NumPixels);

    // Convert input to FColor (BGRA)
    if (SrcFormat == TSF_BGRA8)
    {
        FMemory::Memcpy(SrcColors.GetData(), SrcData, NumPixels * sizeof(FColor));
    }
    else if (SrcFormat == TSF_G8)
    {
        const uint8* GrayData = SrcData;
        for (int32 i = 0; i < NumPixels; ++i)
        {
            uint8 Val = GrayData[i];
            SrcColors[i] = FColor(Val, Val, Val, 255);
        }
    }
    else
    {
        UE_LOG(LogTexturePacker, Warning, TEXT("Unsupported Source Format: %d for texture: %s"), (int32)SrcFormat, *SourceTex->GetName());
        FMemory::Memset(SrcColors.GetData(), 0, NumPixels * sizeof(FColor));
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
    uint8* DestData = ResultData.GetData();
    for (int32 i = 0; i < TargetSize * TargetSize; ++i)
    {
        const FColor& C = ResizedColors[i];
        DestData[i * 4 + 0] = C.R;
        DestData[i * 4 + 1] = C.G;
        DestData[i * 4 + 2] = C.B;
        DestData[i * 4 + 3] = C.A;
    }
#else
    UE_LOG(LogTexturePacker, Error, TEXT("TextureChannelPacker requires WITH_EDITORONLY_DATA to access Source."));
#endif

    return ResultData;
}

void FTextureChannelPackerModule::CreateTexture(const FString& PackageName, int32 Resolution)
{
    // Create the package
    UPackage* Package = CreatePackage(*PackageName);
    Package->FullyLoad();

    // Create the Texture2D
    FName TextureName = FName(*FPaths::GetBaseFilename(PackageName));
    UTexture2D* NewTexture = NewObject<UTexture2D>(Package, TextureName, RF_Public | RF_Standalone | RF_MarkAsRootSet);

    // Initialize PlatformData
    FTexturePlatformData* PlatformData = new FTexturePlatformData();
    PlatformData->SizeX = Resolution;
    PlatformData->SizeY = Resolution;
    PlatformData->PixelFormat = PF_B8G8R8A8;

    // Allocate Mip
    FTexture2DMipMap* Mip = new FTexture2DMipMap();
    PlatformData->Mips.Add(Mip);
    Mip->SizeX = Resolution;
    Mip->SizeY = Resolution;

    // Get Resized Data for Inputs
    TArray<uint8> DataR = GetResizedTextureData(InputTextureR.Get(), Resolution);
    TArray<uint8> DataG = GetResizedTextureData(InputTextureG.Get(), Resolution);
    TArray<uint8> DataB = GetResizedTextureData(InputTextureB.Get(), Resolution);
    TArray<uint8> DataA = GetResizedTextureData(InputTextureA.Get(), Resolution);

    bool bHasAlpha = InputTextureA.IsValid();

    // Lock and Write Pixels
    Mip->BulkData.Lock(LOCK_READ_WRITE);
    uint8* TextureData = (uint8*)Mip->BulkData.Realloc(Resolution * Resolution * 4);

    for (int32 i = 0; i < Resolution * Resolution; ++i)
    {
        // Each Data array is RGBA.
        // New.R = InputR_Data[i].R (Byte 0)
        // New.G = InputG_Data[i].R (Byte 0)
        // New.B = InputB_Data[i].R (Byte 0)

        uint8 R_Val = DataR[i * 4 + 0];
        uint8 G_Val = DataG[i * 4 + 0];
        uint8 B_Val = DataB[i * 4 + 0];
        uint8 A_Val = bHasAlpha ? DataA[i * 4 + 0] : 255; // Use Red channel of Alpha input, or 255

        // Output Texture is PF_B8G8R8A8 (BGRA memory layout)
        TextureData[i * 4 + 0] = B_Val; // B
        TextureData[i * 4 + 1] = G_Val; // G
        TextureData[i * 4 + 2] = R_Val; // R
        TextureData[i * 4 + 3] = A_Val; // A
    }

    Mip->BulkData.Unlock();

    // Assign PlatformData
#if WITH_EDITORONLY_DATA
    NewTexture->Source.Init(Resolution, Resolution, 1, 1, TSF_BGRA8);
    uint8* NewSourceData = NewTexture->Source.LockMip(0);
    if (NewSourceData)
    {
        FMemory::Memcpy(NewSourceData, TextureData, Resolution * Resolution * 4);
    }
    NewTexture->Source.UnlockMip(0);
#endif
    NewTexture->SetPlatformData(PlatformData);

    // Final settings
    NewTexture->CompressionSettings = GetSelectedCompressionSettings();
    NewTexture->SRGB = false;
    NewTexture->UpdateResource();

    Package->MarkPackageDirty();
    FAssetRegistryModule::AssetCreated(NewTexture);

    ShowNotification(FText::Format(LOCTEXT("SuccessTextureSaved", "Texture Saved: {0}"), FText::FromString(PackageName)), true);
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
