[日本語 (Japanese)](API.ja.md)

# TextureChannelPacker API Documentation

This document provides technical details about the `TextureChannelPacker` module, intended for developers who wish to understand the internal architecture, extend functionality, or contribute to the project.

## Overview

The **TextureChannelPacker** is an Editor-only plugin module that provides a Slate-based UI tool for packing individual grayscale or color textures (Red, Green, Blue, Alpha) into a single RGBA texture asset.

### Key Features
- **Editor Integration**: Integrated into the Level Editor "Tools" menu.
- **Thread Safety**: Uses explicit data extraction and reconstruction steps to safely handle `UTexture2D` resources on the Game Thread.
- **Parallel Processing**: Uses `ParallelFor` to process and resize texture channels concurrently.
- **Smart Naming**: Automatically generates output filenames based on input assets.

## Module Architecture

The module is implemented as a single primary class, `FTextureChannelPackerModule`, which inherits from `IModuleInterface`.

*   **Source Path**: `Plugins/TextureChannelPacker/Source/TextureChannelPacker/`
*   **Header**: `Public/TextureChannelPacker.h`
*   **Implementation**: `Private/TextureChannelPacker.cpp`

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

### Data Structures

To support multi-threaded processing without accessing `UObject` methods (like `LockMip`) from background threads, the module uses two helper structs defined in `TextureChannelPacker.cpp`:

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
    -   **Resizing**: If the input resolution differs from the `TargetResolution`, `FImageUtils::ImageResize` is used.

3.  **Reconstruction (Game Thread)**
    -   A new `UTexture2D` is created (or updated) in the package.
    -   The `Source` mip is locked for writing.
    -   Data from the 4 `FTextureProcessResult` arrays is interleaved into the final `BGRA8` memory layout.
    -   `UpdateResource()` and `PostEditChange()` are called to finalize the asset.

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
