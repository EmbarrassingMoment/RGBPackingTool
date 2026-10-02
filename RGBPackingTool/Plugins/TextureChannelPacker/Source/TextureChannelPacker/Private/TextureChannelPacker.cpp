#include "TextureChannelPacker.h"
#include "TextureChannelPackerShared.h"
#include "TextureChannelPackerCore.h"
#include "TextureChannelUnpacker.h"
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
#include "Styling/AppStyle.h"
#include "Logging/LogMacros.h"
#include "PropertyCustomizationHelpers.h"
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
#include "Math/UnrealMathUtility.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Images/SImage.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "ThumbnailRendering/ThumbnailManager.h"
#include "Async/ParallelFor.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/Input/SSegmentedControl.h"
#include "Widgets/SNullWidget.h"
#include "Styling/SlateBrush.h"

#define LOCTEXT_NAMESPACE "FTextureChannelPackerModule"

using namespace TextureChannelPackerUtils;

static const FName TextureChannelPackerTabName("TextureChannelPacker");

FText FCompressionOption::GetDisplayName() const
{
    return GetLocalizedMessage(InternalName, DisplayNameEn, DisplayNameJa);
}

// ========== Source Channel Helpers ==========

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

/** Short label for the preview view-mode dropdown. */
static FString PreviewModeToShortString(EPreviewMode Mode)
{
    switch (Mode)
    {
    case EPreviewMode::RGB:   return TEXT("RGB");
    case EPreviewMode::Red:   return TEXT("R");
    case EPreviewMode::Green: return TEXT("G");
    case EPreviewMode::Blue:  return TEXT("B");
    case EPreviewMode::Alpha: return TEXT("A");
    }
    return TEXT("RGB");
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
    Json->SetStringField(TEXT("UnpackSuffixR"), UnpackSuffixR);
    Json->SetStringField(TEXT("UnpackSuffixG"), UnpackSuffixG);
    Json->SetStringField(TEXT("UnpackSuffixB"), UnpackSuffixB);
    Json->SetStringField(TEXT("UnpackSuffixA"), UnpackSuffixA);
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

    // Unpack suffix fields are optional for backward compatibility with pre-1.8.0 presets;
    // missing fields keep the "_R"/"_G"/"_B"/"_A" defaults.
    FString Suffix;
    if (JsonObject->TryGetStringField(TEXT("UnpackSuffixR"), Suffix)) Preset.UnpackSuffixR = Suffix;
    if (JsonObject->TryGetStringField(TEXT("UnpackSuffixG"), Suffix)) Preset.UnpackSuffixG = Suffix;
    if (JsonObject->TryGetStringField(TEXT("UnpackSuffixB"), Suffix)) Preset.UnpackSuffixB = Suffix;
    if (JsonObject->TryGetStringField(TEXT("UnpackSuffixA"), Suffix)) Preset.UnpackSuffixA = Suffix;

    return Preset;
}

void FTextureChannelPackerModule::StartupModule()
{
    // Initialize Compression Options (shared with headless callers via TextureChannelPackerCore)
    for (FCompressionOption& Option : TextureChannelPackerCore::GetCompressionOptions())
    {
        CompressionOptions.Add(MakeShared<FCompressionOption>(MoveTemp(Option)));
    }

    CurrentCompressionOption = CompressionOptions[0];

    // Initialize source channel options (shared by all four input slots)
    SourceChannelOptions.Add(MakeShared<ESourceChannel>(ESourceChannel::Red));
    SourceChannelOptions.Add(MakeShared<ESourceChannel>(ESourceChannel::Green));
    SourceChannelOptions.Add(MakeShared<ESourceChannel>(ESourceChannel::Blue));
    SourceChannelOptions.Add(MakeShared<ESourceChannel>(ESourceChannel::Alpha));

    // Initialize preview view-mode options (RGB composite + isolated channels)
    PreviewModeOptions.Add(MakeShared<EPreviewMode>(EPreviewMode::RGB));
    PreviewModeOptions.Add(MakeShared<EPreviewMode>(EPreviewMode::Red));
    PreviewModeOptions.Add(MakeShared<EPreviewMode>(EPreviewMode::Green));
    PreviewModeOptions.Add(MakeShared<EPreviewMode>(EPreviewMode::Blue));
    PreviewModeOptions.Add(MakeShared<EPreviewMode>(EPreviewMode::Alpha));

    // Initialize Built-in Presets
    InitializeBuiltInPresets();
    LoadPresetsFromDisk();

    // Set ORM as the default active preset (index 1, after Custom)
    if (Presets.Num() > 1)
    {
        CurrentPreset = Presets[1]; // ORM
        CurrentFileNameSuffix = CurrentPreset->FileNameSuffix;
    }

    // Create the Unpack tab implementation. It shares the module-owned preset list and
    // defaults to the same preset as the Pack tab (ORM).
    Unpacker = MakeShared<FTextureChannelUnpacker>();
    Unpacker->Initialize(&Presets, CurrentPreset);

    // Register Nomad Tab
    FGlobalTabmanager::Get()->RegisterNomadTabSpawner(TextureChannelPackerTabName, FOnSpawnTab::CreateRaw(this, &FTextureChannelPackerModule::OnSpawnPluginTab))
        .SetDisplayName(LOCTEXT("TextureChannelPackerTabTitle", "Texture Channel Packer"))
        .SetMenuType(ETabSpawnerMenuType::Hidden)
        .SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Layout"));

    // Menus are registered once ToolMenus is ready. ToolMenus does not run startup callbacks
    // while its UI is disabled (e.g. in commandlets), so headless runs leave the menus alone.
    UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FTextureChannelPackerModule::RegisterMenus));
}

void FTextureChannelPackerModule::RegisterMenus()
{
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
    UToolMenus::UnRegisterStartupCallback(this);
    UToolMenus::UnregisterOwner(this);
    FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(TextureChannelPackerTabName);

    // Release preview resources.
    if (PreviewBrush.IsValid())
    {
        PreviewBrush->SetResourceObject(nullptr);
    }
    PreviewBrush.Reset();
    PreviewTexture.Reset();

    // Release the Unpack tab resources.
    if (Unpacker.IsValid())
    {
        Unpacker->ReleaseResources();
        Unpacker.Reset();
    }
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

    // Pack mode content (this module's own UI).
    TSharedRef<SWidget> PackContent =
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

                // Preview Section
                + SScrollBox::Slot()
                .Padding(10.0f, 5.0f)
                [
                    SNew(SVerticalBox)
                    // Header row: label + view-mode dropdown + Update button
                    + SVerticalBox::Slot()
                    .AutoHeight()
                    .Padding(0.0f, 0.0f, 0.0f, 6.0f)
                    [
                        SNew(SHorizontalBox)
                        + SHorizontalBox::Slot()
                        .FillWidth(1.0f)
                        .VAlign(VAlign_Center)
                        [
                            SNew(STextBlock)
                            .Text(GetLocalizedMessage(TEXT("PreviewLabel"), TEXT("Preview"), TEXT("プレビュー")))
                            .Font(FAppStyle::GetFontStyle("PropertyWindow.BoldFont"))
                        ]
                        // View-mode label
                        + SHorizontalBox::Slot()
                        .AutoWidth()
                        .VAlign(VAlign_Center)
                        .Padding(0.0f, 0.0f, 4.0f, 0.0f)
                        [
                            SNew(STextBlock)
                            .Text(GetLocalizedMessage(TEXT("PreviewViewLabel"), TEXT("View"), TEXT("表示")))
                            .Font(FAppStyle::GetFontStyle("PropertyWindow.NormalFont"))
                        ]
                        // View-mode dropdown (RGB / R / G / B / A)
                        + SHorizontalBox::Slot()
                        .AutoWidth()
                        .VAlign(VAlign_Center)
                        .Padding(0.0f, 0.0f, 8.0f, 0.0f)
                        [
                            SNew(SComboBox<TSharedPtr<EPreviewMode>>)
                            .ToolTipText(GetLocalizedMessage(
                                TEXT("PreviewViewTooltip"),
                                TEXT("Choose what the preview shows: the RGB composite, or a single channel (R/G/B/A) as grayscale. Switching is instant and does not re-read the textures."),
                                TEXT("プレビューの表示内容を選択します: RGB合成、または単一チャンネル(R/G/B/A)のグレースケール表示。切り替えは即時で、テクスチャの再読み込みは行いません。")
                            ))
                            .OptionsSource(&PreviewModeOptions)
                            .OnSelectionChanged_Lambda([this](TSharedPtr<EPreviewMode> NewSelection, ESelectInfo::Type SelectInfo)
                            {
                                if (NewSelection.IsValid() && SelectInfo != ESelectInfo::Direct)
                                {
                                    PreviewMode = *NewSelection;
                                    if (bPreviewValid)
                                    {
                                        RebuildPreviewTexture();
                                    }
                                }
                            })
                            .OnGenerateWidget_Lambda([](TSharedPtr<EPreviewMode> Item)
                            {
                                return SNew(STextBlock).Text(FText::FromString(PreviewModeToShortString(*Item)));
                            })
                            [
                                SNew(STextBlock)
                                .Text_Lambda([this]()
                                {
                                    return FText::FromString(PreviewModeToShortString(PreviewMode));
                                })
                            ]
                        ]
                        + SHorizontalBox::Slot()
                        .AutoWidth()
                        [
                            SNew(SButton)
                            .Text(GetLocalizedMessage(TEXT("UpdatePreviewButton"), TEXT("Update Preview"), TEXT("プレビュー更新")))
                            .ToolTipText(GetLocalizedMessage(
                                TEXT("UpdatePreviewTooltip"),
                                TEXT("Build a low-resolution preview of the packed result using the current inputs and settings. Use the View dropdown to inspect individual channels."),
                                TEXT("現在の入力と設定でパック結果の低解像度プレビューを生成します。「表示」ドロップダウンで各チャンネルを個別に確認できます。")
                            ))
                            .OnClicked_Lambda([this]()
                            {
                                UpdatePreview();
                                return FReply::Handled();
                            })
                        ]
                    ]
                    // Preview image (visible once generated)
                    + SVerticalBox::Slot()
                    .AutoHeight()
                    .HAlign(HAlign_Center)
                    [
                        SNew(SBox)
                        .Visibility_Lambda([this]()
                        {
                            return bPreviewValid ? EVisibility::Visible : EVisibility::Collapsed;
                        })
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
                                .Image_Lambda([this]() -> const FSlateBrush*
                                {
                                    return PreviewBrush.IsValid() ? PreviewBrush.Get() : nullptr;
                                })
                            ]
                        ]
                    ]
                    // Hint text (visible before first preview)
                    + SVerticalBox::Slot()
                    .AutoHeight()
                    .HAlign(HAlign_Center)
                    [
                        SNew(STextBlock)
                        .Visibility_Lambda([this]()
                        {
                            return bPreviewValid ? EVisibility::Collapsed : EVisibility::Visible;
                        })
                        .Text(GetLocalizedMessage(
                            TEXT("PreviewHint"),
                            TEXT("Click \"Update Preview\" to see the packed result."),
                            TEXT("「プレビュー更新」を押すとパック結果を表示します。")
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
            ];

    // Unpack mode content (owned by FTextureChannelUnpacker).
    TSharedRef<SWidget> UnpackContent = Unpacker.IsValid()
        ? Unpacker->CreateContent()
        : TSharedRef<SWidget>(SNullWidget::NullWidget);

    return SNew(SDockTab)
        .TabRole(ETabRole::NomadTab)
        [
            SNew(SVerticalBox)

            // Pack / Unpack mode switcher
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(10.0f, 8.0f, 10.0f, 0.0f)
            .HAlign(HAlign_Center)
            [
                SNew(SSegmentedControl<int32>)
                .Value_Lambda([this]()
                {
                    return ActiveTabIndex;
                })
                .OnValueChanged_Lambda([this](int32 NewIndex)
                {
                    ActiveTabIndex = NewIndex;
                })
                + SSegmentedControl<int32>::Slot(0)
                .Text(GetLocalizedMessage(TEXT("PackTabLabel"), TEXT("Pack"), TEXT("パック")))
                .ToolTip(GetLocalizedMessage(
                    TEXT("PackTabTooltip"),
                    TEXT("Pack multiple textures into the channels of a single RGBA texture."),
                    TEXT("複数のテクスチャを1枚のRGBAテクスチャのチャンネルにパックします。")))
                + SSegmentedControl<int32>::Slot(1)
                .Text(GetLocalizedMessage(TEXT("UnpackTabLabel"), TEXT("Unpack"), TEXT("アンパック")))
                .ToolTip(GetLocalizedMessage(
                    TEXT("UnpackTabTooltip"),
                    TEXT("Extract the R/G/B/A channels of a packed texture into separate grayscale textures."),
                    TEXT("パック済みテクスチャの R/G/B/A チャンネルを個別のグレースケールテクスチャとして抽出します。")))
            ]

            // Active mode content
            + SVerticalBox::Slot()
            .FillHeight(1.0f)
            [
                SNew(SWidgetSwitcher)
                .WidgetIndex_Lambda([this]()
                {
                    return ActiveTabIndex;
                })
                + SWidgetSwitcher::Slot()
                [
                    PackContent
                ]
                + SWidgetSwitcher::Slot()
                [
                    UnpackContent
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

    // Same naming rule as the core, so the overwrite check below covers the asset that is written.
    const FString PackageName = TextureChannelPackerCore::MakePackageName(OutputPackagePath, OutputFileName);

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

    OutputFileName = TextureChannelPackerCore::MakePackedAssetName(InputNames, CurrentFileNameSuffix);
}

void FTextureChannelPackerModule::CreateTexture(const FString& PackageName, int32 Width, int32 Height)
{
    check(IsInGameThread());

    // Split after the last '/' and keep the slash on the path, so MakePackageName rebuilds exactly
    // PackageName (the name OnGenerateClicked checked for an existing asset).
    int32 LastSlashIndex = INDEX_NONE;
    PackageName.FindLastChar(TEXT('/'), LastSlashIndex);

    FChannelPackerPackRequest Request = BuildPackRequest();
    Request.OutputPath = PackageName.Left(LastSlashIndex + 1);
    Request.OutputName = PackageName.Mid(LastSlashIndex + 1);
    Request.Width = Width;
    Request.Height = Height;

    // OnGenerateClicked has already confirmed any overwrite, and the editor workflow leaves the
    // new package dirty for the user to save.
    Request.OverwritePolicy = EChannelPackerOverwritePolicy::Overwrite;
    Request.bSave = false;

    TextureChannelPackerCore::FExecutionOptions Options;
    Options.bShowProgressDialog = true;

    const FChannelPackerResult Result = TextureChannelPackerCore::Pack(Request, Options);

    // Unreadable inputs were packed with the slot default; warn about each one.
    for (const FChannelPackerWarning& Warning : Result.Warnings)
    {
        ShowNotification(FText::FromString(Warning.Message), false);
    }

    if (Result.Status == EChannelPackerStatus::Succeeded)
    {
        FText FormatPattern = GetLocalizedMessage(TEXT("SuccessTextureSaved"), TEXT("Texture Saved: {0}"), TEXT("テクスチャを保存しました: {0}"));
        ShowNotification(FText::Format(FormatPattern, FText::FromString(PackageName)), true);
    }
    else
    {
        ShowNotification(FText::FromString(Result.Message), false);
    }
}

FChannelPackerPackRequest FTextureChannelPackerModule::BuildPackRequest() const
{
    FChannelPackerPackRequest Request;

    Request.Red.Texture = InputTextureR.Get();
    Request.Green.Texture = InputTextureG.Get();
    Request.Blue.Texture = InputTextureB.Get();
    Request.Alpha.Texture = InputTextureA.Get();

    Request.Red.SourceChannel = SourceChannelR;
    Request.Green.SourceChannel = SourceChannelG;
    Request.Blue.SourceChannel = SourceChannelB;
    Request.Alpha.SourceChannel = SourceChannelA;

    Request.Red.bInvert = bInvertR;
    Request.Green.bInvert = bInvertG;
    Request.Blue.bInvert = bInvertB;
    Request.Alpha.bInvert = bInvertA;

    Request.Width = TargetWidth;
    Request.Height = TargetHeight;
    Request.OutputPath = OutputPackagePath;
    Request.OutputName = OutputFileName;
    Request.FileNameSuffix = CurrentFileNameSuffix;
    Request.Compression = CurrentCompressionOption.IsValid() ? CurrentCompressionOption->InternalName : FString(TEXT("Masks"));
    return Request;
}

void FTextureChannelPackerModule::UpdatePreview()
{
    check(IsInGameThread());

    // Derive a small preview resolution from the target aspect ratio so the preview stays
    // cheap to build even when the output target is very large (e.g. 16K).
    const int32 PreviewMaxDim = 256;
    const int32 SrcW = FMath::Clamp(TargetWidth, 1, MaxTextureDimension);
    const int32 SrcH = FMath::Clamp(TargetHeight, 1, MaxTextureDimension);
    const float Scale = FMath::Min(1.0f, (float)PreviewMaxDim / (float)FMath::Max(SrcW, SrcH));
    const int32 W = FMath::Max(1, FMath::RoundToInt(SrcW * Scale));
    const int32 H = FMath::Max(1, FMath::RoundToInt(SrcH * Scale));
    const int32 NumPixels = W * H;

    FPhaseTimer TotalTimer(TEXT("Preview total"));

    // Lock the source mips read-only on the Game Thread, then process channels in parallel.
    // The preview reads the sources in place; nothing is copied at full resolution.
    FLockedTextureSource RawInputs[4];
    {
        FPhaseTimer ExtractTimer(TEXT("Preview extract"));
        RawInputs[0] = FLockedTextureSource::Lock(InputTextureR.Get());
        RawInputs[1] = FLockedTextureSource::Lock(InputTextureG.Get());
        RawInputs[2] = FLockedTextureSource::Lock(InputTextureB.Get());
        RawInputs[3] = FLockedTextureSource::Lock(InputTextureA.Get());
    }

    const ESourceChannel SourceChannels[4] = { SourceChannelR, SourceChannelG, SourceChannelB, SourceChannelA };
    FTextureProcessResult Results[4];
    {
        FPhaseTimer ProcessTimer(TEXT("Preview process"));
        ParallelFor(4, [&](int32 Index)
        {
            Results[Index] = ProcessTextureSourceData(RawInputs[Index], W, H, SourceChannels[Index]);
        });
    }

    const bool bInvert[4] = { bInvertR, bInvertG, bInvertB, bInvertA };

    // Default value for an empty/invalid channel: RGB default to black (0), Alpha to opaque
    // (255), matching generation. Invert is applied afterwards, also matching generation.
    const uint8 DefaultValues[4] = { 0, 0, 0, 255 };

    // Resolve each channel into a W*H byte array and cache it. Caching the per-channel data
    // lets the view mode switch instantly without re-reading the source textures.
    for (int32 Index = 0; Index < 4; ++Index)
    {
        TArray<uint8>& Data = PreviewChannels[Index];
        if (Results[Index].ProcessedData.Num() == NumPixels)
        {
            Data = MoveTemp(Results[Index].ProcessedData);
        }
        else
        {
            Data.Init(DefaultValues[Index], NumPixels);
        }
        if (bInvert[Index])
        {
            for (uint8& Value : Data)
            {
                Value = 255 - Value;
            }
        }
    }

    PreviewDisplayWidth = W;
    PreviewDisplayHeight = H;
    bPreviewValid = true;

    RebuildPreviewTexture();
}

void FTextureChannelPackerModule::RebuildPreviewTexture()
{
    check(IsInGameThread());

    if (!bPreviewValid)
    {
        return;
    }

    const int32 W = PreviewDisplayWidth;
    const int32 H = PreviewDisplayHeight;
    const int32 NumPixels = W * H;
    if (NumPixels <= 0 || PreviewChannels[0].Num() != NumPixels)
    {
        return;
    }

    const TArray<uint8>& R = PreviewChannels[0];
    const TArray<uint8>& G = PreviewChannels[1];
    const TArray<uint8>& B = PreviewChannels[2];
    const TArray<uint8>& A = PreviewChannels[3];

    // Compose the BGRA buffer for the selected view mode. Single-channel modes show that
    // channel as grayscale (B=G=R); RGB shows the composite. The displayed alpha is always
    // forced opaque so the preview is never blended against the panel background.
    TArray<uint8> BGRA;
    BGRA.SetNumUninitialized(NumPixels * 4);
    for (int32 i = 0; i < NumPixels; ++i)
    {
        uint8 OutB, OutG, OutR;
        switch (PreviewMode)
        {
        case EPreviewMode::Red:   OutB = OutG = OutR = R[i]; break;
        case EPreviewMode::Green: OutB = OutG = OutR = G[i]; break;
        case EPreviewMode::Blue:  OutB = OutG = OutR = B[i]; break;
        case EPreviewMode::Alpha: OutB = OutG = OutR = A[i]; break;
        case EPreviewMode::RGB:
        default:                  OutB = B[i]; OutG = G[i]; OutR = R[i]; break;
        }

        const int32 Offset = i * 4;
        BGRA[Offset + 0] = OutB;
        BGRA[Offset + 1] = OutG;
        BGRA[Offset + 2] = OutR;
        BGRA[Offset + 3] = 255;
    }

    // Build a transient texture for display. PF_B8G8R8A8 matches our BGRA byte order.
    UTexture2D* NewPreview = UTexture2D::CreateTransient(W, H, PF_B8G8R8A8);
    if (!NewPreview)
    {
        UE_LOG(LogTexturePacker, Warning, TEXT("Failed to create transient preview texture."));
        return;
    }

    // Show the literal packed values (linear), matching the generated asset's color space.
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
    PreviewTexture.Reset(NewPreview);

    if (!PreviewBrush.IsValid())
    {
        PreviewBrush = MakeShared<FSlateBrush>();
        PreviewBrush->DrawAs = ESlateBrushDrawType::Image;
    }
    PreviewBrush->SetResourceObject(PreviewTexture.Get());
    PreviewBrush->ImageSize = FVector2D(W, H);
}

void FTextureChannelPackerModule::ShowNotification(const FText& Message, bool bSuccess)
{
    // Explicit qualification: the unqualified name would recurse into this member.
    TextureChannelPackerUtils::ShowNotification(Message, bSuccess);
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
    for (FChannelPackerPreset& Preset : TextureChannelPackerCore::GetBuiltInPresets())
    {
        TSharedPtr<FChannelPackerPreset> PresetPtr = MakeShared<FChannelPackerPreset>(MoveTemp(Preset));

        // "Custom" is the sentinel preset and always comes first.
        if (!CustomPreset.IsValid())
        {
            CustomPreset = PresetPtr;
        }
        Presets.Add(PresetPtr);
    }
}

void FTextureChannelPackerModule::LoadPresetsFromDisk()
{
    TArray<FChannelPackerPreset> UserPresets = TextureChannelPackerCore::LoadUserPresets();
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

        // The Pack tab has no UI for unpack suffixes; inherit them from the preset being customized.
        NewPreset.UnpackSuffixR = CurrentPreset->UnpackSuffixR;
        NewPreset.UnpackSuffixG = CurrentPreset->UnpackSuffixG;
        NewPreset.UnpackSuffixB = CurrentPreset->UnpackSuffixB;
        NewPreset.UnpackSuffixA = CurrentPreset->UnpackSuffixA;
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
    if (TextureChannelPackerCore::SaveUserPreset(NewPreset))
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

        // Keep the Unpack tab's preset dropdown in sync.
        if (Unpacker.IsValid())
        {
            Unpacker->OnPresetListChanged();
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
    TextureChannelPackerCore::DeleteUserPreset(PresetName);

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

    // Keep the Unpack tab's preset dropdown in sync (it falls back if it was using the deleted preset).
    if (Unpacker.IsValid())
    {
        Unpacker->OnPresetListChanged();
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
