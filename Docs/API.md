[日本語 (Japanese)](API.ja.md)

# TextureChannelPacker API Documentation

This document provides technical details about the `TextureChannelPacker` module, intended for developers who wish to understand the internal architecture, extend functionality, or contribute to the project.

## Overview

The **TextureChannelPacker** is an Editor-only plugin module that provides a Slate-based UI tool for packing individual grayscale or color textures (Red, Green, Blue, Alpha) into a single RGBA texture asset. Since v1.8.0 it also provides the reverse operation: an **Unpack** tab that splits a packed texture into per-channel grayscale assets.

### Key Features
- **Editor Integration**: Integrated into the Level Editor "Tools" menu.
- **Thread Safety**: Uses explicit data extraction and reconstruction steps to safely handle `UTexture2D` resources on the Game Thread.
- **Parallel Processing**: Uses `ParallelFor` to process and resize texture channels concurrently.
- **Smart Naming**: Automatically generates output filenames based on input assets.
- **Pack / Unpack Modes**: A segmented control at the top of the dock tab switches between the two workflows (`SWidgetSwitcher`).

## Module Architecture

The module's primary class is `FTextureChannelPackerModule` (inherits `IModuleInterface`), which owns the dock tab and the Pack workflow. The Unpack workflow lives in a separate class, `FTextureChannelUnpacker`, instantiated by the module at startup. Processing code shared by both workflows sits in the `TextureChannelPackerUtils` namespace.

*   **Source Path**: `Plugins/TextureChannelPacker/Source/TextureChannelPacker/`
*   **Header**: `Public/TextureChannelPacker.h`
*   **Implementation**: `Private/TextureChannelPacker.cpp`
*   **Shared Utilities**: `Private/TextureChannelPackerShared.h` / `.cpp` (namespace `TextureChannelPackerUtils`)
*   **Unpack Tab**: `Private/TextureChannelUnpacker.h` / `.cpp` (class `FTextureChannelUnpacker`)

### Public Interface

The public interface is minimal, as the module is primarily consumed via the Editor UI.

```cpp
class FTextureChannelPackerModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
};
```

- `StartupModule`: Registers the "TextureChannelPacker" tab spawner and extends the "Tools" menu.
- `ShutdownModule`: Unregisters the tab spawner and cleans up menu extensions.

## Internal Implementation

### Core Class: `FTextureChannelPackerModule`

This class manages the UI state, holds references to input textures, and executes the packing logic.

#### Key Methods

*   **`OnSpawnPluginTab`**: Constructs the main Slate UI.
*   **`CreateChannelInputSlot`**: Helper method to create consistent UI widgets for each channel input (Label + Object Picker).
*   **`OnGenerateClicked`**: Validates user inputs (e.g., ensuring at least one texture is selected and resolution is valid) before triggering generation.
*   **`CreateTexture`**: The main driver for the texture generation process.
*   **`AutoGenerateFileName`**: heuristic logic to determine a suitable output filename based on the Longest Common Prefix of inputs.

### Unpack Class: `FTextureChannelUnpacker`

Owns all Unpack tab state and UI. Created in `StartupModule` and kept alive until `ShutdownModule`, so its widget lambdas can safely capture the raw `this` pointer (the same lifetime contract the module uses).

#### Key Methods

*   **`CreateContent`**: Builds the Unpack tab Slate UI (preset dropdown, source picker, 2×2 channel grid, output settings, Unpack button).
*   **`UpdatePreview`**: Extracts the source once, runs uniform-value detection per channel on the full-resolution raw data (early-exits on the first differing pixel), auto-unchecks uniform ("possibly unused") channels, then builds four grayscale preview textures at a capped resolution.
*   **`OnExtractClicked`**: Validates inputs, confirms overwrites (single dialog listing all affected assets), then writes one `TSF_G8` texture asset per selected channel (`TC_Grayscale`, `SRGB = false`) at the source resolution.
*   **`AutoGenerateBaseName`**: Strips any known packed suffix (every preset's `FileNameSuffix`) from the source name and enforces the `T_` prefix. Per-channel output names are `<Base><UnpackSuffix>` using the selected preset's suffixes.

The Unpack tab shares the module-owned preset array (`FChannelPackerPreset` gained `UnpackSuffixR/G/B/A` fields; older JSON files load with `_R`/`_G`/`_B`/`_A` defaults). The module calls `OnPresetListChanged` after saving/deleting presets to keep the Unpack dropdown in sync.

### Data Structures

To support multi-threaded processing without accessing `UObject` methods (like `LockMip`) from background threads, the module uses two helper structs defined in the `TextureChannelPackerUtils` namespace (`TextureChannelPackerShared.h`):

#### `FTextureRawData`
Used to transport raw pixel data from the Game Thread to worker threads.
```cpp
struct FTextureRawData
{
    TArray<uint8> RawData;      // Raw byte content of Mip 0
    int32 Width;                // Texture Width
    int32 Height;               // Texture Height
    ETextureSourceFormat Format;// e.g., TSF_BGRA8, TSF_G8
    FString TextureName;        // For logging/debugging
    bool bIsValid;              // True if extraction succeeded
};
```

#### `FTextureProcessResult`
Used to return processed single-channel data from worker threads to the Game Thread.
```cpp
struct FTextureProcessResult
{
    TArray<uint8> ProcessedData; // Always 8-bit single channel (0-255)
    FText ErrorMessage;          // Error message if failed
    bool bSuccess;               // Success flag
};
```

## Processing Flow

The texture generation pipeline (`CreateTexture`) is designed to be responsive and thread-safe.

1.  **Extraction (Game Thread)**
    -   `ExtractTextureSourceData` is called for each input (R, G, B, A).
    -   It locks the `Source` mipmap of the `UTexture2D` and `Memcpy`s the raw bytes into `FTextureRawData`.
    -   This isolates the background threads from UObject validity checks.

2.  **Processing (Parallel Threads)**
    -   `ParallelFor` is used to invoke `ProcessTextureSourceData` for all 4 channels concurrently.
    -   **Format Conversion**: Supports `TSF_BGRA8` (extracts Red), `TSF_G8` (Grayscale), `TSF_G16` (16-bit Grayscale), and Float formats (`TSF_R16F`, `TSF_R32F`, `TSF_RGBA32F`). All are converted to 8-bit `uint8`.
    -   **Resizing**: If the input resolution differs from the `TargetWidth` and `TargetHeight`, `FImageUtils::ImageResize` is used.

3.  **Reconstruction (Game Thread)**
    -   A new `UTexture2D` is created (or updated) in the package.
    -   The `Source` mip is locked for writing.
    -   Data from the 4 `FTextureProcessResult` arrays is interleaved into the final `BGRA8` memory layout.
    -   `UpdateResource()` and `PostEditChange()` are called to finalize the asset.

### Unpack Flow

The unpack pipeline (`FTextureChannelUnpacker::OnExtractClicked`) reuses the same extraction/processing primitives:

1.  **Extraction (Game Thread)**: `ExtractTextureSourceData` is called once for the single source texture.
2.  **Channel Extraction (Parallel Threads)**: `ProcessTextureSourceData` runs once per selected channel at the source resolution. Because all tasks share one `FTextureRawData`, they pass `bCanConsumeInput = false` so the same-resolution `TSF_G8` fast path copies instead of moving the shared buffer (the Pack flow keeps the zero-copy move, since each channel there owns its own raw data).
3.  **Asset Creation (Game Thread)**: For each selected channel, a `TSF_G8` source is initialized, the channel bytes are memcpy'd in, and the asset is finalized with `TC_Grayscale` compression and `SRGB = false`.

## Extension Points

### Adding New Compression Settings
Modify `StartupModule` to add new entries to `CompressionOptions`.
```cpp
CompressionOptions.Add(MakeShared<FString>("My New Setting"));
```
Then update `GetSelectedCompressionSettings` to return the appropriate `TextureCompressionSettings` enum.

### Supporting New Input Formats
Update the `switch(Input.Format)` block in `ProcessTextureSourceData` to handle additional `ETextureSourceFormat` types (e.g., `TSF_BC1`).

### Localization
The module uses `LOCTEXT_NAMESPACE` and a helper function `GetLocalizedMessage` to support English and Japanese. All new user-facing strings should use this pattern to maintain bilingual support.
