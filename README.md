[日本語 (Japanese)](README.ja.md)

# TextureChannelPacker

**TextureChannelPacker** is a powerful plugin for Unreal Engine 5.7 designed to efficiently pack individual grayscale textures into the Red, Green, Blue, and Alpha channels of a single output texture. It is an essential tool for creating ORM (Occlusion, Roughness, Metallic) maps and optimizing your project's texture usage.

## Features

- **4-Channel Packing (RGBA)**: Takes up to four input textures and packs their Red channels into the output's Red, Green, Blue, and Alpha channels respectively.
- **Smart File Naming**: Automatically suggests an optimized output filename based on your input textures. It intelligently identifies common prefixes (e.g., `T_Rock_AO`, `T_Rock_R` -> `T_Rock_ORM`) to save you typing time.
- **Auto-Resizing**: Automatically resizes input textures to match the specified target resolution using High-Quality Bilinear Interpolation.
- **Extended Format Support**: Full support for **8-bit**, **16-bit Grayscale** (Heightmaps), and **32-bit Float** (SDF/LUT) source formats.
- **UI Localization**: The interface seamlessly switches between **English** and **Japanese** based on your Editor's language settings.
- **UX Improvements**:
  - **Drag & Drop**: Drag textures directly from the Content Browser into input slots.
  - **Path Picker**: Easily select your output directory using the folder icon button.
  - **Toast Notifications**: Get clear, non-intrusive feedback (Success/Error) instantly.

## Why use this tool?

If you are new to optimization, here is why channel packing is important:

- **Texture Sampler Limits**: Materials usually have a limit of 16 texture samplers. Packing 3 textures (AO, Roughness, Metallic) into 1 file saves sampler slots, preventing "Texture Sampler out of bounds" errors.
- **Memory Efficiency**: Loading one combined file uses less memory overhead than loading three separate files.
- **File Management**: It keeps your Content Browser organized by significantly reducing the number of texture assets.

## Installation

1. **Clone or Download**:
   Download this repository and place the `TextureChannelPacker` folder into your project's `Plugins` directory.
   - If the `Plugins` folder does not exist, create it in your project's root directory.

   Structure:
   ```
   MyProject/
   ├── Plugins/
   │   └── TextureChannelPacker/
   ```

2. **Generate Project Files**:
   Right-click your `.uproject` file and select **Generate Visual Studio project files**.

3. **Build**:
   Open the `.sln` file and build your project to compile the plugin.

4. **Enable**:
   Launch Unreal Engine. Go to **Edit > Plugins**, search for **TextureChannelPacker**, and enable it.

## Usage Guide

### Step 1: Open Tool
Navigate to the main menu bar and select **Tools > Texture Packing**. The tool window can be docked anywhere in your editor.

### Step 2: Assign Inputs
- **Drag & Drop** textures from the Content Browser into the Red, Green, Blue, or Alpha slots.
- **Red/Green/Blue**: Typically used for Ambient Occlusion, Roughness, and Metallic.
- **Alpha**: Optional. Defaults to White (Opaque) if left empty.

### Step 3: Configure Output
- **Smart Naming**: The **File Name** field is automatically generated based on your inputs (e.g., adding `_ORM` suffix), but you can manually edit it if needed.
- **Output Path**: Use the **Path Picker** (folder icon) to select the destination folder.
- **Resolution**: Set your desired output size (Valid range: 1 - 8192).
- **Compression Settings**: Select the appropriate compression from the dropdown (see table below).

### Step 4: Generate
Click **Generate Texture**.
- A **Toast Notification** will appear to confirm the success or alert you of any errors.
- The new asset will be created immediately in the Content Browser.

## Technical Details

### Supported Formats
- **8-bit**: `BGRA8`, `G8` (Standard textures).
- **16-bit**: `G16` (High-precision Heightmaps).
- **Float**: `R16F`, `R32F`, `RGBA32F` (SDFs, LUTs).

### Limitations
- **Clamping**: Floating point values (from SDFs or Heightmaps) are **clamped** to the `0.0 - 1.0` range during packing. Values outside this range will be clipped.

## Compression Settings

| Setting | Best For | Description |
| :--- | :--- | :--- |
| **Masks (Recommended)** | **ORM Maps**, Packed Masks | Disables sRGB. Prevents color artifacts between channels. Best for standard PBR workflows. |
| **Grayscale** | **Height Maps**, Single Masks | Keeps values linear. Good for single-channel data. |
| **Default** | Color Textures | Standard compression. Usually for Albedo/Diffuse. Not recommended for channel packing. |

## License

This project is licensed under the MIT License.
