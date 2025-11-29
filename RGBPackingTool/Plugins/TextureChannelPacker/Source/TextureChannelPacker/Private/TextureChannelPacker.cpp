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
#include "Styling/AppStyle.h"
#include "Logging/LogMacros.h"
#include "PropertyCustomizationHelpers.h"
#include "Engine/Texture2D.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/Paths.h"

#define LOCTEXT_NAMESPACE "FTextureChannelPackerModule"

DEFINE_LOG_CATEGORY_STATIC(LogTexturePacker, Log, All);

static const FName TextureChannelPackerTabName("TextureChannelPacker");

void FTextureChannelPackerModule::StartupModule()
{
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
                    SNew(SEditableTextBox)
                    .Text_Lambda([this] { return FText::FromString(OutputPackagePath); })
                    .OnTextCommitted_Lambda([this](const FText& NewText, ETextCommit::Type) { OutputPackagePath = NewText.ToString(); })
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
    UE_LOG(LogTexturePacker, Log, TEXT("Resolution: %d"), TargetResolution);
    UE_LOG(LogTexturePacker, Log, TEXT("Output Path: %s"), *OutputPackagePath);
    UE_LOG(LogTexturePacker, Log, TEXT("File Name: %s"), *OutputFileName);

    FString PackageName = OutputPackagePath;
    if (!PackageName.EndsWith(TEXT("/")))
    {
        PackageName += TEXT("/");
    }
    PackageName += OutputFileName;

    CreateTexture(PackageName, TargetResolution);

    return FReply::Handled();
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

    // Lock and Write Pixels
    Mip->BulkData.Lock(LOCK_READ_WRITE);
    uint8* TextureData = (uint8*)Mip->BulkData.Realloc(Resolution * Resolution * 4);

    for (int32 i = 0; i < Resolution * Resolution; ++i)
    {
        // BGRA
        TextureData[i * 4 + 0] = 0;   // B
        TextureData[i * 4 + 1] = 0;   // G
        TextureData[i * 4 + 2] = 255; // R
        TextureData[i * 4 + 3] = 255; // A
    }

    Mip->BulkData.Unlock();

    // Assign PlatformData
#if WITH_EDITORONLY_DATA
    NewTexture->Source.Init(Resolution, Resolution, 1, 1, TSF_BGRA8);
#endif
    NewTexture->SetPlatformData(PlatformData);

    // Final settings
    NewTexture->CompressionSettings = TC_Masks;
    NewTexture->SRGB = false;
    NewTexture->UpdateResource();

    Package->MarkPackageDirty();
    FAssetRegistryModule::AssetCreated(NewTexture);
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FTextureChannelPackerModule, TextureChannelPacker)
