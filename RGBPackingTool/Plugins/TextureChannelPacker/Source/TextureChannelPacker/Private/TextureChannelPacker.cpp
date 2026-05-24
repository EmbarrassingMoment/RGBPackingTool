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
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformFileManager.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"

#define LOCTEXT_NAMESPACE "FTextureChannelPackerModule"

DEFINE_LOG_CATEGORY_STATIC(LogTexturePacker, Log, All);

static const FName TextureChannelPackerTabName("TextureChannelPacker");

static constexpr int32 LargeTextureWarningThreshold = 8192;
static constexpr int32 MaxTextureDimension = 16384;

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

FText FCompressionOption::GetDisplayName() const
{
    return GetLocalizedMessage(InternalName, DisplayNameEn, DisplayNameJa);
}

// ========== Source Channel Helpers ==========

/** Short single-letter label for a source channel (R/G/B/A) for compact UI. */
static FString SourceChannelToShortString(ESourceChannel Ch)
{
    switch (Ch)
    {
    case ESourceChannel::Red:   return TEXT("R");
    case ESourceChannel::Green: return TEXT("G");
    case ESourceChannel::Blue:  return TEXT("B");
    case ESourceChannel::Alpha: return TEXT("A");
    }
    return TEXT("R");
}

/** Stable string id for serialization. */
static FString SourceChannelToId(ESourceChannel Ch)
{
    return SourceChannelToShortString(Ch);
}

/** Inverse of SourceChannelToId. Returns Red if the id is unrecognized. */
static ESourceChannel SourceChannelFromId(const FString& Id)
{
    if (Id == TEXT("G")) return ESourceChannel::Green;
    if (Id == TEXT("B")) return ESourceChannel::Blue;
    if (Id == TEXT("A")) return ESourceChannel::Alpha;
    return ESourceChannel::Red;
}

// ========== FChannelPackerPreset Implementation ==========

FText FChannelPackerPreset::GetDisplayName() const
{
    return FText::FromString(PresetName);
}

TSharedPtr<FJsonObject> FChannelPackerPreset::ToJson() const
{
    TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
    Json->SetNumberField(TEXT("Version"), 1);
    Json->SetStringField(TEXT("PresetName"), PresetName);
    Json->SetStringField(TEXT("RedLabelEn"), RedLabelEn);
    Json->SetStringField(TEXT("RedLabelJa"), RedLabelJa);
    Json->SetStringField(TEXT("GreenLabelEn"), GreenLabelEn);
    Json->SetStringField(TEXT("GreenLabelJa"), GreenLabelJa);
    Json->SetStringField(TEXT("BlueLabelEn"), BlueLabelEn);
    Json->SetStringField(TEXT("BlueLabelJa"), BlueLabelJa);
    Json->SetStringField(TEXT("AlphaLabelEn"), AlphaLabelEn);
    Json->SetStringField(TEXT("AlphaLabelJa"), AlphaLabelJa);
    Json->SetStringField(TEXT("FileNameSuffix"), FileNameSuffix);
    Json->SetStringField(TEXT("DefaultCompressionName"), DefaultCompressionName);
    Json->SetBoolField(TEXT("DefaultInvertR"), bDefaultInvertR);
    Json->SetBoolField(TEXT("DefaultInvertG"), bDefaultInvertG);
    Json->SetBoolField(TEXT("DefaultInvertB"), bDefaultInvertB);
    Json->SetBoolField(TEXT("DefaultInvertA"), bDefaultInvertA);
    Json->SetStringField(TEXT("DefaultSourceChannelR"), SourceChannelToId(DefaultSourceChannelR));
    Json->SetStringField(TEXT("DefaultSourceChannelG"), SourceChannelToId(DefaultSourceChannelG));
    Json->SetStringField(TEXT("DefaultSourceChannelB"), SourceChannelToId(DefaultSourceChannelB));
    Json->SetStringField(TEXT("DefaultSourceChannelA"), SourceChannelToId(DefaultSourceChannelA));
    return Json;
}

FChannelPackerPreset FChannelPackerPreset::FromJson(const TSharedPtr<FJsonObject>& JsonObject)
{
    FChannelPackerPreset Preset;
    if (!JsonObject.IsValid())
    {
        return Preset;
    }
    Preset.PresetName = JsonObject->GetStringField(TEXT("PresetName"));
    Preset.bIsBuiltIn = false;
    Preset.RedLabelEn = JsonObject->GetStringField(TEXT("RedLabelEn"));
    Preset.RedLabelJa = JsonObject->GetStringField(TEXT("RedLabelJa"));
    Preset.GreenLabelEn = JsonObject->GetStringField(TEXT("GreenLabelEn"));
    Preset.GreenLabelJa = JsonObject->GetStringField(TEXT("GreenLabelJa"));
    Preset.BlueLabelEn = JsonObject->GetStringField(TEXT("BlueLabelEn"));
    Preset.BlueLabelJa = JsonObject->GetStringField(TEXT("BlueLabelJa"));
    Preset.AlphaLabelEn = JsonObject->GetStringField(TEXT("AlphaLabelEn"));
    Preset.AlphaLabelJa = JsonObject->GetStringField(TEXT("AlphaLabelJa"));
    Preset.FileNameSuffix = JsonObject->GetStringField(TEXT("FileNameSuffix"));
    Preset.DefaultCompressionName = JsonObject->GetStringField(TEXT("DefaultCompressionName"));
    Preset.bDefaultInvertR = JsonObject->GetBoolField(TEXT("DefaultInvertR"));
    Preset.bDefaultInvertG = JsonObject->GetBoolField(TEXT("DefaultInvertG"));
    Preset.bDefaultInvertB = JsonObject->GetBoolField(TEXT("DefaultInvertB"));
    Preset.bDefaultInvertA = JsonObject->GetBoolField(TEXT("DefaultInvertA"));

    // Source channel fields are optional for backward compatibility with v1.5.0 presets;
    // missing fields default to Red.
    FString ChannelId;
    if (JsonObject->TryGetStringField(TEXT("DefaultSourceChannelR"), ChannelId)) Preset.DefaultSourceChannelR = SourceChannelFromId(ChannelId);
    if (JsonObject->TryGetStringField(TEXT("DefaultSourceChannelG"), ChannelId)) Preset.DefaultSourceChannelG = SourceChannelFromId(ChannelId);
    if (JsonObject->TryGetStringField(TEXT("DefaultSourceChannelB"), ChannelId)) Preset.DefaultSourceChannelB = SourceChannelFromId(ChannelId);
    if (JsonObject->TryGetStringField(TEXT("DefaultSourceChannelA"), ChannelId)) Preset.DefaultSourceChannelA = SourceChannelFromId(ChannelId);

    return Preset;
}

// ========== Preset Persistence Helpers ==========

static FString GetPresetsDirectory()
{
    return FPaths::ProjectSavedDir() / TEXT("TextureChannelPacker") / TEXT("Presets");
}

static FString SanitizePresetFileName(const FString& Name)
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

static TArray<FChannelPackerPreset> LoadUserPresetsFromDisk()
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

static bool SavePresetToDisk(const FChannelPackerPreset& Preset)
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

static bool DeletePresetFromDisk(const FString& PresetName)
{
    FString PresetsDir = GetPresetsDirectory();
    FString FileName = SanitizePresetFileName(PresetName) + TEXT(".json");
    FString FilePath = PresetsDir / FileName;
    return IFileManager::Get().Delete(*FilePath);
}

void FTextureChannelPackerModule::StartupModule()
{
    // Initialize Compression Options
    FCompressionOption MasksOption;
    MasksOption.InternalName = "Masks";
    MasksOption.CompressionSetting = TC_Masks;
    MasksOption.DisplayNameEn = "Masks (Recommended)";
    MasksOption.DisplayNameJa = "マスク (推奨)";
    CompressionOptions.Add(MakeShared<FCompressionOption>(MasksOption));

    FCompressionOption GrayscaleOption;
    GrayscaleOption.InternalName = "Grayscale";
    GrayscaleOption.CompressionSetting = TC_Grayscale;
    GrayscaleOption.DisplayNameEn = "Grayscale";
    GrayscaleOption.DisplayNameJa = "グレースケール";
    CompressionOptions.Add(MakeShared<FCompressionOption>(GrayscaleOption));

    FCompressionOption DefaultOption;
    DefaultOption.InternalName = "Default";
    DefaultOption.CompressionSetting = TC_Default;
    DefaultOption.DisplayNameEn = "Default";
    DefaultOption.DisplayNameJa = "デフォルト";
    CompressionOptions.Add(MakeShared<FCompressionOption>(DefaultOption));

    CurrentCompressionOption = CompressionOptions[0];

    // Initialize source channel options (shared by all four input slots)
    SourceChannelOptions.Add(MakeShared<ESourceChannel>(ESourceChannel::Red));
    SourceChannelOptions.Add(MakeShared<ESourceChannel>(ESourceChannel::Green));
    SourceChannelOptions.Add(MakeShared<ESourceChannel>(ESourceChannel::Blue));
    SourceChannelOptions.Add(MakeShared<ESourceChannel>(ESourceChannel::Alpha));

    // Initialize Built-in Presets
    InitializeBuiltInPresets();
    LoadPresetsFromDisk();

    // Set ORM as the default active preset (index 1, after Custom)
    if (Presets.Num() > 1)
    {
        CurrentPreset = Presets[1]; // ORM
        CurrentFileNameSuffix = CurrentPreset->FileNameSuffix;
    }

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

TSharedRef<SWidget> FTextureChannelPackerModule::CreateChannelInputSlot(const TAttribute<FText>& LabelText, TWeakObjectPtr<UTexture2D>& TargetTexturePtr, bool& bInvertFlag, ESourceChannel& SourceChannelRef, const FText& TooltipText)
{
    // Capture the address of the member variable to update it inside the lambda
    TWeakObjectPtr<UTexture2D>* TexturePtr = &TargetTexturePtr;
    bool* InvertPtr = &bInvertFlag;
    ESourceChannel* SourceChannelPtr = &SourceChannelRef;

    TSharedPtr<STextBlock> LabelWidget = SNew(STextBlock)
        .Text(LabelText)
        .Font(FAppStyle::GetFontStyle("PropertyWindow.NormalFont"));

    if (!TooltipText.IsEmpty())
    {
        LabelWidget->SetToolTipText(TooltipText);
    }

    const FText SourceChannelTooltip = GetLocalizedMessage(
        TEXT("SourceChannelTooltip"),
        TEXT("Which channel of the input texture to read from. Useful when sourcing data from a color or already-packed texture. Ignored for single-channel grayscale formats."),
        TEXT("入力テクスチャから読み取るチャンネルを選択します。カラーテクスチャや既存のパック済みテクスチャから特定チャンネルを取り出す場合に便利です。グレースケール形式では無視されます。")
    );

    return SNew(SVerticalBox)
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(0.0f, 0.0f, 0.0f, 4.0f)
        [
            SNew(SHorizontalBox)
            // Label
            + SHorizontalBox::Slot()
            .FillWidth(1.0f)
            .VAlign(VAlign_Center)
            [
                LabelWidget.ToSharedRef()
            ]
            // "Source" Label
            + SHorizontalBox::Slot()
            .AutoWidth()
            .VAlign(VAlign_Center)
            .Padding(0.0f, 0.0f, 4.0f, 0.0f)
            [
                SNew(STextBlock)
                .Text(GetLocalizedMessage(TEXT("SourceChannelLabel"), TEXT("Source"), TEXT("ソース")))
                .ToolTipText(SourceChannelTooltip)
                .Font(FAppStyle::GetFontStyle("PropertyWindow.NormalFont"))
            ]
            // Source Channel Dropdown
            + SHorizontalBox::Slot()
            .AutoWidth()
            .VAlign(VAlign_Center)
            .Padding(0.0f, 0.0f, 8.0f, 0.0f)
            [
                SNew(SComboBox<TSharedPtr<ESourceChannel>>)
                .ToolTipText(SourceChannelTooltip)
                .OptionsSource(&SourceChannelOptions)
                .OnSelectionChanged_Lambda([this, SourceChannelPtr](TSharedPtr<ESourceChannel> NewSelection, ESelectInfo::Type SelectInfo)
                {
                    if (NewSelection.IsValid() && SelectInfo != ESelectInfo::Direct)
                    {
                        *SourceChannelPtr = *NewSelection;
                        MarkCustomIfChanged();
                    }
                })
                .OnGenerateWidget_Lambda([](TSharedPtr<ESourceChannel> Item)
                {
                    return SNew(STextBlock).Text(FText::FromString(SourceChannelToShortString(*Item)));
                })
                [
                    SNew(STextBlock)
                    .Text_Lambda([SourceChannelPtr]()
                    {
                        return FText::FromString(SourceChannelToShortString(*SourceChannelPtr));
                    })
                ]
            ]
            // Checkbox
            + SHorizontalBox::Slot()
            .AutoWidth()
            .VAlign(VAlign_Center)
            [
                SNew(SCheckBox)
                .IsChecked_Lambda([InvertPtr]()
                {
                    return *InvertPtr ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
                })
                .OnCheckStateChanged_Lambda([this, InvertPtr](ECheckBoxState NewState)
                {
                    *InvertPtr = (NewState == ECheckBoxState::Checked);
                    MarkCustomIfChanged();
                })
            ]
            // "Invert" Label
            + SHorizontalBox::Slot()
            .AutoWidth()
            .VAlign(VAlign_Center)
            .Padding(4.0f, 0.0f, 0.0f, 0.0f)
            [
                SNew(STextBlock)
                .Text(LOCTEXT("InvertLabel", "Invert"))
                .Font(FAppStyle::GetFontStyle("PropertyWindow.NormalFont"))
            ]
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

            // Scrollable content area
            + SVerticalBox::Slot()
            .FillHeight(1.0f)
            [
                SNew(SScrollBox)

                // Preset Selection
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
                        SNew(SHorizontalBox)
                        // Preset Dropdown
                        + SHorizontalBox::Slot()
                        .FillWidth(1.0f)
                        [
                            SAssignNew(PresetComboBox, SComboBox<TSharedPtr<FChannelPackerPreset>>)
                            .OptionsSource(&Presets)
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
                        // Save As Button
                        + SHorizontalBox::Slot()
                        .AutoWidth()
                        .Padding(4.0f, 0.0f, 0.0f, 0.0f)
                        [
                            SNew(SButton)
                            .Text(GetLocalizedMessage(TEXT("SaveAsButton"), TEXT("Save As..."), TEXT("名前を付けて保存...")))
                            .OnClicked_Lambda([this]()
                            {
                                // Create a simple input dialog window
                                TSharedRef<SWindow> SaveWindow = SNew(SWindow)
                                    .Title(GetLocalizedMessage(TEXT("SavePresetTitle"), TEXT("Save Preset"), TEXT("プリセットを保存")))
                                    .ClientSize(FVector2D(400.0f, 130.0f))
                                    .SupportsMinimize(false)
                                    .SupportsMaximize(false);

                                TSharedPtr<SEditableTextBox> NameInput;

                                SaveWindow->SetContent(
                                    SNew(SVerticalBox)
                                    + SVerticalBox::Slot()
                                    .AutoHeight()
                                    .Padding(10.0f)
                                    [
                                        SNew(STextBlock)
                                        .Text(GetLocalizedMessage(TEXT("EnterPresetName"), TEXT("Enter Preset Name:"), TEXT("プリセット名を入力:")))
                                        .Font(FAppStyle::GetFontStyle("PropertyWindow.NormalFont"))
                                    ]
                                    + SVerticalBox::Slot()
                                    .AutoHeight()
                                    .Padding(10.0f, 0.0f)
                                    [
                                        SAssignNew(NameInput, SEditableTextBox)
                                        .Text(CurrentPreset.IsValid() && !CurrentPreset->bIsBuiltIn
                                            ? FText::FromString(CurrentPreset->PresetName)
                                            : FText::GetEmpty())
                                    ]
                                    + SVerticalBox::Slot()
                                    .AutoHeight()
                                    .Padding(10.0f)
                                    .HAlign(HAlign_Right)
                                    [
                                        SNew(SHorizontalBox)
                                        + SHorizontalBox::Slot()
                                        .AutoWidth()
                                        .Padding(0.0f, 0.0f, 4.0f, 0.0f)
                                        [
                                            SNew(SButton)
                                            .Text(LOCTEXT("OKButton", "OK"))
                                            .OnClicked_Lambda([this, SaveWindow, NameInput]()
                                            {
                                                FString Name = NameInput->GetText().ToString().TrimStartAndEnd();
                                                if (!Name.IsEmpty())
                                                {
                                                    SaveCurrentAsPreset(Name);
                                                }
                                                FSlateApplication::Get().RequestDestroyWindow(SaveWindow);
                                                return FReply::Handled();
                                            })
                                        ]
                                        + SHorizontalBox::Slot()
                                        .AutoWidth()
                                        [
                                            SNew(SButton)
                                            .Text(LOCTEXT("CancelButton", "Cancel"))
                                            .OnClicked_Lambda([SaveWindow]()
                                            {
                                                FSlateApplication::Get().RequestDestroyWindow(SaveWindow);
                                                return FReply::Handled();
                                            })
                                        ]
                                    ]
                                );

                                FSlateApplication::Get().AddModalWindow(SaveWindow, FSlateApplication::Get().GetActiveTopLevelWindow());
                                return FReply::Handled();
                            })
                        ]
                        // Delete Button
                        + SHorizontalBox::Slot()
                        .AutoWidth()
                        .Padding(4.0f, 0.0f, 0.0f, 0.0f)
                        [
                            SNew(SButton)
                            .Text(GetLocalizedMessage(TEXT("DeleteButton"), TEXT("Delete"), TEXT("削除")))
                            .IsEnabled_Lambda([this]()
                            {
                                return CurrentPreset.IsValid() && !CurrentPreset->bIsBuiltIn;
                            })
                            .OnClicked_Lambda([this]()
                            {
                                if (CurrentPreset.IsValid() && !CurrentPreset->bIsBuiltIn)
                                {
                                    FText Msg = FText::Format(
                                        GetLocalizedMessage(
                                            TEXT("ConfirmDeletePreset"),
                                            TEXT("Are you sure you want to delete the preset \"{0}\"?"),
                                            TEXT("プリセット「{0}」を削除しますか？")
                                        ),
                                        FText::FromString(CurrentPreset->PresetName)
                                    );
                                    EAppReturnType::Type Result = FMessageDialog::Open(EAppMsgType::YesNo, Msg);
                                    if (Result == EAppReturnType::Yes)
                                    {
                                        DeleteCurrentPreset();
                                    }
                                }
                                return FReply::Handled();
                            })
                        ]
                    ]
                ]

                // Separator (after Preset)
                + SScrollBox::Slot()
                .Padding(10.0f, 5.0f)
                [
                    SNew(SSeparator)
                ]

                // Red Channel Input
                + SScrollBox::Slot()
                .Padding(10.0f)
                [
                    CreateChannelInputSlot(
                        TAttribute<FText>::Create(TAttribute<FText>::FGetter::CreateRaw(this, &FTextureChannelPackerModule::GetCurrentRedLabel)),
                        InputTextureR,
                        bInvertR,
                        SourceChannelR
                    )
                ]

                // Green Channel Input
                + SScrollBox::Slot()
                .Padding(10.0f)
                [
                    CreateChannelInputSlot(
                        TAttribute<FText>::Create(TAttribute<FText>::FGetter::CreateRaw(this, &FTextureChannelPackerModule::GetCurrentGreenLabel)),
                        InputTextureG,
                        bInvertG,
                        SourceChannelG
                    )
                ]

                // Blue Channel Input
                + SScrollBox::Slot()
                .Padding(10.0f)
                [
                    CreateChannelInputSlot(
                        TAttribute<FText>::Create(TAttribute<FText>::FGetter::CreateRaw(this, &FTextureChannelPackerModule::GetCurrentBlueLabel)),
                        InputTextureB,
                        bInvertB,
                        SourceChannelB
                    )
                ]

                // Alpha Channel Input (with Tooltip)
                + SScrollBox::Slot()
                .Padding(10.0f)
                [
                    CreateChannelInputSlot(
                        TAttribute<FText>::Create(TAttribute<FText>::FGetter::CreateRaw(this, &FTextureChannelPackerModule::GetCurrentAlphaLabel)),
                        InputTextureA,
                        bInvertA,
                        SourceChannelA,
                        GetLocalizedMessage(
                            TEXT("AlphaChannelTooltip"),
                            TEXT("If left empty, fills with White (255) to ensure opacity. Assign a texture to pack a custom Alpha mask."),
                            TEXT("空の場合は白 (255) で塗りつぶされ、不透明になります。独自のアルファマスクを使用する場合はテクスチャを指定してください。")
                        )
                    )
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

                // Resolution
                + SScrollBox::Slot()
                .Padding(10.0f, 5.0f)
                [
                    SNew(SVerticalBox)
                    + SVerticalBox::Slot()
                    .AutoHeight()
                    .Padding(0.0f, 0.0f, 0.0f, 4.0f)
                    [
                        SNew(STextBlock)
                        .Text(LOCTEXT("ResolutionLabel", "Resolution - Width \u00D7 Height (e.g. 2048 \u00D7 2048)"))
                        .ToolTipText(FText::Format(GetLocalizedMessage(TEXT("ResolutionTooltip"), TEXT("Valid range: 1 - {0} each."), TEXT("有効範囲: それぞれ 1 - {0}")), FText::AsNumber(MaxTextureDimension)))
                        .Font(FAppStyle::GetFontStyle("PropertyWindow.NormalFont"))
                    ]
                    + SVerticalBox::Slot()
                    .AutoHeight()
                    [
                        SNew(SHorizontalBox)
                        // Width
                        + SHorizontalBox::Slot()
                        .FillWidth(1.0f)
                        [
                            SNew(SNumericEntryBox<int32>)
                            .Value_Lambda([this] { return TargetWidth; })
                            .OnValueChanged_Lambda([this](int32 NewValue) { TargetWidth = NewValue; })
                            .AllowSpin(true)
                            .MinValue(1)
                            .MaxValue(MaxTextureDimension)
                            .MinSliderValue(1)
                            .MaxSliderValue(MaxTextureDimension)
                        ]
                        // "×" Separator
                        + SHorizontalBox::Slot()
                        .AutoWidth()
                        .VAlign(VAlign_Center)
                        .Padding(8.0f, 0.0f)
                        [
                            SNew(STextBlock)
                            .Text(FText::FromString(TEXT("\u00D7")))  // Unicode multiplication sign ×
                            .Font(FAppStyle::GetFontStyle("PropertyWindow.NormalFont"))
                        ]
                        // Height
                        + SHorizontalBox::Slot()
                        .FillWidth(1.0f)
                        [
                            SNew(SNumericEntryBox<int32>)
                            .Value_Lambda([this] { return TargetHeight; })
                            .OnValueChanged_Lambda([this](int32 NewValue) { TargetHeight = NewValue; })
                            .AllowSpin(true)
                            .MinValue(1)
                            .MaxValue(MaxTextureDimension)
                            .MinSliderValue(1)
                            .MaxSliderValue(MaxTextureDimension)
                        ]
                    ]
                ]

                // Compression Settings
                + SScrollBox::Slot()
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
                        SNew(SComboBox<TSharedPtr<FCompressionOption>>)
                        .ToolTipText(GetLocalizedMessage(
                            TEXT("CompressionTooltip"),
                            TEXT("Select the compression method for the output texture.\n- Masks: Best for ORM (Occlusion, Roughness, Metallic) or other packed data. (Linear, no sRGB)\n- Grayscale: Best for single-channel values like Height or Alpha masks. (Linear)\n- Default: Standard compression. Not recommended for packed masks."),
                            TEXT("出力テクスチャの圧縮方式を選択します。\n- Masks: ORM (Occlusion, Roughness, Metallic) やパック済みデータに最適 (リニア, sRGBなし)\n- Grayscale: ハイトマップや単一マスクなど1チャンネルの値に最適 (リニア)\n- Default: 標準圧縮。パック済みマスクには非推奨")
                        ))
                        .OptionsSource(&CompressionOptions)
                        .OnSelectionChanged_Lambda([this](TSharedPtr<FCompressionOption> NewSelection, ESelectInfo::Type)
                        {
                            if (NewSelection.IsValid())
                            {
                                CurrentCompressionOption = NewSelection;
                                MarkCustomIfChanged();
                            }
                        })
                        .OnGenerateWidget_Lambda([](TSharedPtr<FCompressionOption> Item)
                        {
                            return SNew(STextBlock).Text(Item->GetDisplayName());
                        })
                        [
                            SNew(STextBlock)
                            .Text_Lambda([this]()
                            {
                                return CurrentCompressionOption.IsValid() ? CurrentCompressionOption->GetDisplayName() : FText::GetEmpty();
                            })
                        ]
                    ]
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

                // File Name
                + SScrollBox::Slot()
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
            ] // end SScrollBox

            // Generate Button (pinned at bottom, always visible)
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
    UE_LOG(LogTexturePacker, Log, TEXT("Invert R: %s, G: %s, B: %s, A: %s"),
        bInvertR ? TEXT("Yes") : TEXT("No"),
        bInvertG ? TEXT("Yes") : TEXT("No"),
        bInvertB ? TEXT("Yes") : TEXT("No"),
        bInvertA ? TEXT("Yes") : TEXT("No"));
    auto ChannelToString = [](ESourceChannel Ch) -> const TCHAR*
    {
        switch (Ch)
        {
        case ESourceChannel::Red:   return TEXT("R");
        case ESourceChannel::Green: return TEXT("G");
        case ESourceChannel::Blue:  return TEXT("B");
        case ESourceChannel::Alpha: return TEXT("A");
        default:                    return TEXT("?");
        }
    };
    UE_LOG(LogTexturePacker, Log, TEXT("Source Channel R<-%s, G<-%s, B<-%s, A<-%s"),
        ChannelToString(SourceChannelR),
        ChannelToString(SourceChannelG),
        ChannelToString(SourceChannelB),
        ChannelToString(SourceChannelA));
    UE_LOG(LogTexturePacker, Log, TEXT("Resolution: %d x %d"), TargetWidth, TargetHeight);
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
    if (TargetWidth < 1 || TargetWidth > MaxTextureDimension || TargetHeight < 1 || TargetHeight > MaxTextureDimension)
    {
        FText Msg = FText::Format(GetLocalizedMessage(TEXT("ErrorInvalidResolution"), TEXT("Width and Height must each be between 1 and {0}."), TEXT("幅と高さはそれぞれ 1 から {0} の間で指定してください。")), FText::AsNumber(MaxTextureDimension));
        ShowNotification(Msg, false);
        return FReply::Handled();
    }

    // Memory Consumption Warning Check
    if (TargetWidth > LargeTextureWarningThreshold || TargetHeight > LargeTextureWarningThreshold)
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

    CreateTexture(PackageName, TargetWidth, TargetHeight);

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

    OutputFileName = BaseName + CurrentFileNameSuffix;
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
 * @brief Returns the byte offset of the requested channel within a BGRA8 pixel.
 *
 * BGRA8 lays out bytes as [B, G, R, A], so Red=2, Green=1, Blue=0, Alpha=3.
 */
static int32 GetBGRAChannelOffset(ESourceChannel Channel)
{
    switch (Channel)
    {
    case ESourceChannel::Red:   return 2;
    case ESourceChannel::Green: return 1;
    case ESourceChannel::Blue:  return 0;
    case ESourceChannel::Alpha: return 3;
    default:                    return 2;
    }
}

/**
 * @brief Extracts the requested channel value from an FColor.
 */
static uint8 ExtractChannelFromFColor(const FColor& C, ESourceChannel Channel)
{
    switch (Channel)
    {
    case ESourceChannel::Red:   return C.R;
    case ESourceChannel::Green: return C.G;
    case ESourceChannel::Blue:  return C.B;
    case ESourceChannel::Alpha: return C.A;
    default:                    return C.R;
    }
}

/**
 * @brief Returns true for source formats that physically only carry one channel of data.
 *
 * For these formats, the channel selector is meaningless — the lone luminance value is
 * always used regardless of which output channel the user picks.
 */
static bool IsSingleChannelFormat(ETextureSourceFormat Format)
{
    return Format == TSF_G8 || Format == TSF_G16 || Format == TSF_R16F || Format == TSF_R32F;
}

/**
 * @brief Processes raw texture data to produce a single channel of output.
 *
 * This function handles resizing (using FImageUtils) and format conversion (e.g., extracting
 * the selected channel from BGRA/RGBA, or converting 16-bit grayscale to 8-bit).
 * This function is designed to be thread-safe and run in parallel tasks.
 *
 * @param Input The raw source data extracted from the input texture.
 * @param TargetWidth The target width for the output.
 * @param TargetHeight The target height for the output.
 * @param SourceChannel Which channel of the input to read (R/G/B/A). Ignored for single-channel formats.
 * @return FTextureProcessResult The processed single-channel 8-bit data.
 */
static FTextureProcessResult ProcessTextureSourceData(FTextureRawData& Input, int32 TargetWidth, int32 TargetHeight, ESourceChannel SourceChannel)
{
    FTextureProcessResult Result;
    // Default to zero-filled array
    Result.ProcessedData.Init(0, TargetWidth * TargetHeight);

    if (!Input.bIsValid)
    {
        return Result; // Empty/Invalid input results in black channel (or white if handled by caller default)
    }

    int32 SrcWidth = Input.Width;
    int32 SrcHeight = Input.Height;
    int32 NumPixels = SrcWidth * SrcHeight;
    const uint8* SrcData = Input.RawData.GetData();

    // Optimization: Fast path for same-resolution textures
    if (SrcWidth == TargetWidth && SrcHeight == TargetHeight)
    {
        if (Input.Format == TSF_G8)
        {
            // Direct move for Grayscale input (zero-copy optimization).
            // Channel selection is moot for single-channel data.
            Result.ProcessedData = MoveTemp(Input.RawData);
            return Result;
        }
        else if (Input.Format == TSF_BGRA8)
        {
            // Parallel channel extraction for BGRA input
            Result.ProcessedData.SetNumUninitialized(NumPixels);
            uint8* DestData = Result.ProcessedData.GetData();
            const uint8* SrcPtr = SrcData;
            const int32 ChannelOffset = GetBGRAChannelOffset(SourceChannel);

            ParallelFor(NumPixels, [DestData, SrcPtr, ChannelOffset](int32 i)
            {
                DestData[i] = SrcPtr[i * 4 + ChannelOffset];
            });
            return Result;
        }
    }

    TArray<FColor> SrcColors;
    SrcColors.SetNumUninitialized(NumPixels);

    // Convert input to FColor (RGBA values stored in FColor's R/G/B/A members).
    // For single-channel formats we replicate the value across R/G/B so that channel
    // selection still produces the expected result.
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
            uint8 Val = (uint8)(((uint32)GrayData16[i] * 255 + 32767) / 65535);
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
            uint8 Val = (uint8)(FMath::Clamp<float>((float)Pixel16[i] * 255.0f, 0.0f, 255.0f) + 0.5f);
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
            uint8 Val = (uint8)(FMath::Clamp<float>(Pixel32[i] * 255.0f, 0.0f, 255.0f) + 0.5f);
            SrcColors[i] = FColor(Val, Val, Val, 255);
        }
        break;
    }
    case TSF_RGBA32F:
    {
        // Linear Color: 16 bytes per pixel. Preserve all four channels so the user can pick any.
        const FLinearColor* LinearColors = (const FLinearColor*)SrcData;
        for (int32 i = 0; i < NumPixels; ++i)
        {
            const FLinearColor& LC = LinearColors[i];
            uint8 R = (uint8)(FMath::Clamp<float>(LC.R * 255.0f, 0.0f, 255.0f) + 0.5f);
            uint8 G = (uint8)(FMath::Clamp<float>(LC.G * 255.0f, 0.0f, 255.0f) + 0.5f);
            uint8 B = (uint8)(FMath::Clamp<float>(LC.B * 255.0f, 0.0f, 255.0f) + 0.5f);
            uint8 A = (uint8)(FMath::Clamp<float>(LC.A * 255.0f, 0.0f, 255.0f) + 0.5f);
            SrcColors[i] = FColor(R, G, B, A);
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
    if (SrcWidth != TargetWidth || SrcHeight != TargetHeight)
    {
        ResizedColors.SetNum(TargetWidth * TargetHeight);
        FImageUtils::ImageResize(SrcWidth, SrcHeight, SrcColors, TargetWidth, TargetHeight, ResizedColors, false);
    }
    else
    {
        ResizedColors = MoveTemp(SrcColors);
    }

    // For single-channel source formats, the channel selection has no effect (R=G=B).
    // We always read R to keep the inner loop branch-free.
    const ESourceChannel EffectiveChannel = IsSingleChannelFormat(Input.Format) ? ESourceChannel::Red : SourceChannel;

    // Convert FColor to uint8 array (single channel, 1 byte per pixel)
    Result.ProcessedData.SetNumUninitialized(TargetWidth * TargetHeight);
    uint8* DestData = Result.ProcessedData.GetData();
    for (int32 i = 0; i < TargetWidth * TargetHeight; ++i)
    {
        DestData[i] = ExtractChannelFromFColor(ResizedColors[i], EffectiveChannel);
    }

    return Result;
}

void FTextureChannelPackerModule::CreateTexture(const FString& PackageName, int32 Width, int32 Height)
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

    const ESourceChannel SourceChannels[4] = { SourceChannelR, SourceChannelG, SourceChannelB, SourceChannelA };

    ParallelFor(4, [&](int32 Index)
    {
        ProcessedResults[Index] = ProcessTextureSourceData(RawInputs[Index], Width, Height, SourceChannels[Index]);
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
    NewTexture->Source.Init(Width, Height, 1, 1, TSF_BGRA8);

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
        // Invert channels if requested
        auto InvertChannel = [](TArray<uint8>& Data)
        {
            for (int32 i = 0; i < Data.Num(); ++i)
            {
                Data[i] = 255 - Data[i];
            }
        };

        if (bInvertR && ProcessedResults[0].ProcessedData.Num() > 0) InvertChannel(ProcessedResults[0].ProcessedData);
        if (bInvertG && ProcessedResults[1].ProcessedData.Num() > 0) InvertChannel(ProcessedResults[1].ProcessedData);
        if (bInvertB && ProcessedResults[2].ProcessedData.Num() > 0) InvertChannel(ProcessedResults[2].ProcessedData);
        if (bInvertA && ProcessedResults[3].ProcessedData.Num() > 0) InvertChannel(ProcessedResults[3].ProcessedData);

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
            DefaultR.Init(0, Width * Height);
            if (bInvertR) InvertChannel(DefaultR);
            PtrR = DefaultR.GetData();
        }
        if (!PtrG)
        {
            DefaultG.Init(0, Width * Height);
            if (bInvertG) InvertChannel(DefaultG);
            PtrG = DefaultG.GetData();
        }
        if (!PtrB)
        {
            DefaultB.Init(0, Width * Height);
            if (bInvertB) InvertChannel(DefaultB);
            PtrB = DefaultB.GetData();
        }
        if (!PtrA)
        {
            DefaultA.Init(255, Width * Height);
            if (bInvertA) InvertChannel(DefaultA);
            PtrA = DefaultA.GetData();
        }

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
        return CurrentCompressionOption->CompressionSetting;
    }
    return TC_Masks; // Fallback
}

// ========== Preset System Implementation ==========

FText FTextureChannelPackerModule::GetCurrentRedLabel() const
{
    if (CurrentPreset.IsValid())
    {
        return GetLocalizedMessage(TEXT("RedChannelLabel"), CurrentPreset->RedLabelEn, CurrentPreset->RedLabelJa);
    }
    return GetLocalizedMessage(TEXT("RedChannelLabel"), TEXT("Red Channel Input"), TEXT("Red Channel Input"));
}

FText FTextureChannelPackerModule::GetCurrentGreenLabel() const
{
    if (CurrentPreset.IsValid())
    {
        return GetLocalizedMessage(TEXT("GreenChannelLabel"), CurrentPreset->GreenLabelEn, CurrentPreset->GreenLabelJa);
    }
    return GetLocalizedMessage(TEXT("GreenChannelLabel"), TEXT("Green Channel Input"), TEXT("Green Channel Input"));
}

FText FTextureChannelPackerModule::GetCurrentBlueLabel() const
{
    if (CurrentPreset.IsValid())
    {
        return GetLocalizedMessage(TEXT("BlueChannelLabel"), CurrentPreset->BlueLabelEn, CurrentPreset->BlueLabelJa);
    }
    return GetLocalizedMessage(TEXT("BlueChannelLabel"), TEXT("Blue Channel Input"), TEXT("Blue Channel Input"));
}

FText FTextureChannelPackerModule::GetCurrentAlphaLabel() const
{
    if (CurrentPreset.IsValid())
    {
        return GetLocalizedMessage(TEXT("AlphaChannelLabel"), CurrentPreset->AlphaLabelEn, CurrentPreset->AlphaLabelJa);
    }
    return GetLocalizedMessage(TEXT("AlphaChannelLabel"), TEXT("Alpha Channel Input (Optional)"), TEXT("Alpha Channel Input (任意)"));
}

void FTextureChannelPackerModule::InitializeBuiltInPresets()
{
    // Custom (sentinel - always first)
    {
        TSharedPtr<FChannelPackerPreset> Preset = MakeShared<FChannelPackerPreset>();
        Preset->PresetName = TEXT("Custom");
        Preset->bIsBuiltIn = true;
        Preset->RedLabelEn = TEXT("Red Channel Input");
        Preset->RedLabelJa = TEXT("Red Channel Input");
        Preset->GreenLabelEn = TEXT("Green Channel Input");
        Preset->GreenLabelJa = TEXT("Green Channel Input");
        Preset->BlueLabelEn = TEXT("Blue Channel Input");
        Preset->BlueLabelJa = TEXT("Blue Channel Input");
        Preset->AlphaLabelEn = TEXT("Alpha Channel Input (Optional)");
        Preset->AlphaLabelJa = TEXT("Alpha Channel Input (任意)");
        Preset->FileNameSuffix = TEXT("_Packed");
        Preset->DefaultCompressionName = TEXT("Masks");
        CustomPreset = Preset;
        Presets.Add(Preset);
    }

    // ORM (default)
    {
        TSharedPtr<FChannelPackerPreset> Preset = MakeShared<FChannelPackerPreset>();
        Preset->PresetName = TEXT("ORM");
        Preset->bIsBuiltIn = true;
        Preset->RedLabelEn = TEXT("Red Channel Input (e.g. Ambient Occlusion)");
        Preset->RedLabelJa = TEXT("Red Channel Input (例: アンビエントオクルージョン)");
        Preset->GreenLabelEn = TEXT("Green Channel Input (e.g. Roughness)");
        Preset->GreenLabelJa = TEXT("Green Channel Input (例: ラフネス)");
        Preset->BlueLabelEn = TEXT("Blue Channel Input (e.g. Metallic)");
        Preset->BlueLabelJa = TEXT("Blue Channel Input (例: メタリック)");
        Preset->AlphaLabelEn = TEXT("Alpha Channel Input (Optional)");
        Preset->AlphaLabelJa = TEXT("Alpha Channel Input (任意)");
        Preset->FileNameSuffix = TEXT("_ORM");
        Preset->DefaultCompressionName = TEXT("Masks");
        Presets.Add(Preset);
    }

    // MRA
    {
        TSharedPtr<FChannelPackerPreset> Preset = MakeShared<FChannelPackerPreset>();
        Preset->PresetName = TEXT("MRA");
        Preset->bIsBuiltIn = true;
        Preset->RedLabelEn = TEXT("Red Channel Input (e.g. Metallic)");
        Preset->RedLabelJa = TEXT("Red Channel Input (例: メタリック)");
        Preset->GreenLabelEn = TEXT("Green Channel Input (e.g. Roughness)");
        Preset->GreenLabelJa = TEXT("Green Channel Input (例: ラフネス)");
        Preset->BlueLabelEn = TEXT("Blue Channel Input (e.g. Ambient Occlusion)");
        Preset->BlueLabelJa = TEXT("Blue Channel Input (例: アンビエントオクルージョン)");
        Preset->AlphaLabelEn = TEXT("Alpha Channel Input (Optional)");
        Preset->AlphaLabelJa = TEXT("Alpha Channel Input (任意)");
        Preset->FileNameSuffix = TEXT("_MRA");
        Preset->DefaultCompressionName = TEXT("Masks");
        Presets.Add(Preset);
    }
}

void FTextureChannelPackerModule::LoadPresetsFromDisk()
{
    TArray<FChannelPackerPreset> UserPresets = LoadUserPresetsFromDisk();
    for (FChannelPackerPreset& Preset : UserPresets)
    {
        Presets.Add(MakeShared<FChannelPackerPreset>(MoveTemp(Preset)));
    }
}

void FTextureChannelPackerModule::ApplyPreset(TSharedPtr<FChannelPackerPreset> Preset)
{
    if (!Preset.IsValid())
    {
        return;
    }

    CurrentPreset = Preset;
    CurrentFileNameSuffix = Preset->FileNameSuffix;

    // Apply invert flags
    bInvertR = Preset->bDefaultInvertR;
    bInvertG = Preset->bDefaultInvertG;
    bInvertB = Preset->bDefaultInvertB;
    bInvertA = Preset->bDefaultInvertA;

    // Apply source channel selection
    SourceChannelR = Preset->DefaultSourceChannelR;
    SourceChannelG = Preset->DefaultSourceChannelG;
    SourceChannelB = Preset->DefaultSourceChannelB;
    SourceChannelA = Preset->DefaultSourceChannelA;

    // Apply compression option
    for (const TSharedPtr<FCompressionOption>& Option : CompressionOptions)
    {
        if (Option->InternalName == Preset->DefaultCompressionName)
        {
            CurrentCompressionOption = Option;
            break;
        }
    }

    // Reset filename auto-generation
    bFileNameManuallyEdited = false;
    AutoGenerateFileName();

    // Update combo box selection
    if (PresetComboBox.IsValid())
    {
        PresetComboBox->SetSelectedItem(Preset);
    }
}

void FTextureChannelPackerModule::SaveCurrentAsPreset(const FString& Name)
{
    FChannelPackerPreset NewPreset;
    NewPreset.PresetName = Name;
    NewPreset.bIsBuiltIn = false;

    // Capture current labels from the active preset
    if (CurrentPreset.IsValid())
    {
        NewPreset.RedLabelEn = CurrentPreset->RedLabelEn;
        NewPreset.RedLabelJa = CurrentPreset->RedLabelJa;
        NewPreset.GreenLabelEn = CurrentPreset->GreenLabelEn;
        NewPreset.GreenLabelJa = CurrentPreset->GreenLabelJa;
        NewPreset.BlueLabelEn = CurrentPreset->BlueLabelEn;
        NewPreset.BlueLabelJa = CurrentPreset->BlueLabelJa;
        NewPreset.AlphaLabelEn = CurrentPreset->AlphaLabelEn;
        NewPreset.AlphaLabelJa = CurrentPreset->AlphaLabelJa;
    }

    NewPreset.FileNameSuffix = CurrentFileNameSuffix;
    NewPreset.DefaultCompressionName = CurrentCompressionOption.IsValid() ? CurrentCompressionOption->InternalName : TEXT("Masks");
    NewPreset.bDefaultInvertR = bInvertR;
    NewPreset.bDefaultInvertG = bInvertG;
    NewPreset.bDefaultInvertB = bInvertB;
    NewPreset.bDefaultInvertA = bInvertA;
    NewPreset.DefaultSourceChannelR = SourceChannelR;
    NewPreset.DefaultSourceChannelG = SourceChannelG;
    NewPreset.DefaultSourceChannelB = SourceChannelB;
    NewPreset.DefaultSourceChannelA = SourceChannelA;

    // Save to disk
    if (SavePresetToDisk(NewPreset))
    {
        // Check if a preset with the same name already exists (update it)
        bool bFound = false;
        for (TSharedPtr<FChannelPackerPreset>& Existing : Presets)
        {
            if (!Existing->bIsBuiltIn && Existing->PresetName == Name)
            {
                *Existing = NewPreset;
                CurrentPreset = Existing;
                bFound = true;
                break;
            }
        }

        if (!bFound)
        {
            TSharedPtr<FChannelPackerPreset> NewPresetPtr = MakeShared<FChannelPackerPreset>(MoveTemp(NewPreset));
            Presets.Add(NewPresetPtr);
            CurrentPreset = NewPresetPtr;
        }

        // Refresh combo box
        if (PresetComboBox.IsValid())
        {
            PresetComboBox->RefreshOptions();
            PresetComboBox->SetSelectedItem(CurrentPreset);
        }

        FText Msg = FText::Format(
            GetLocalizedMessage(TEXT("PresetSaved"), TEXT("Preset \"{0}\" saved."), TEXT("プリセット「{0}」を保存しました。")),
            FText::FromString(Name)
        );
        ShowNotification(Msg, true);
    }
    else
    {
        FText Msg = GetLocalizedMessage(TEXT("PresetSaveFailed"), TEXT("Failed to save preset."), TEXT("プリセットの保存に失敗しました。"));
        ShowNotification(Msg, false);
    }
}

void FTextureChannelPackerModule::DeleteCurrentPreset()
{
    if (!CurrentPreset.IsValid() || CurrentPreset->bIsBuiltIn)
    {
        return;
    }

    FString PresetName = CurrentPreset->PresetName;

    // Delete from disk
    DeletePresetFromDisk(PresetName);

    // Remove from array
    Presets.Remove(CurrentPreset);

    // Switch to Custom
    CurrentPreset = CustomPreset;
    CurrentFileNameSuffix = CustomPreset->FileNameSuffix;

    // Refresh combo box
    if (PresetComboBox.IsValid())
    {
        PresetComboBox->RefreshOptions();
        PresetComboBox->SetSelectedItem(CustomPreset);
    }

    FText Msg = FText::Format(
        GetLocalizedMessage(TEXT("PresetDeleted"), TEXT("Preset \"{0}\" deleted."), TEXT("プリセット「{0}」を削除しました。")),
        FText::FromString(PresetName)
    );
    ShowNotification(Msg, true);
}

void FTextureChannelPackerModule::MarkCustomIfChanged()
{
    if (!CurrentPreset.IsValid() || CurrentPreset == CustomPreset)
    {
        return;
    }

    // Check if current settings differ from the active preset
    bool bChanged = false;

    if (bInvertR != CurrentPreset->bDefaultInvertR ||
        bInvertG != CurrentPreset->bDefaultInvertG ||
        bInvertB != CurrentPreset->bDefaultInvertB ||
        bInvertA != CurrentPreset->bDefaultInvertA)
    {
        bChanged = true;
    }

    if (SourceChannelR != CurrentPreset->DefaultSourceChannelR ||
        SourceChannelG != CurrentPreset->DefaultSourceChannelG ||
        SourceChannelB != CurrentPreset->DefaultSourceChannelB ||
        SourceChannelA != CurrentPreset->DefaultSourceChannelA)
    {
        bChanged = true;
    }

    if (CurrentCompressionOption.IsValid() &&
        CurrentCompressionOption->InternalName != CurrentPreset->DefaultCompressionName)
    {
        bChanged = true;
    }

    if (bChanged)
    {
        CurrentPreset = CustomPreset;
        if (PresetComboBox.IsValid())
        {
            PresetComboBox->SetSelectedItem(CustomPreset);
        }
    }
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FTextureChannelPackerModule, TextureChannelPacker)
