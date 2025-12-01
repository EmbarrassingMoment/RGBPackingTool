# TextureChannelPacker

**TextureChannelPacker** is an Unreal Engine 5.7 plugin designed to efficiently pack separate grayscale textures into the Red, Green, Blue, and Alpha channels of a single output texture. This is commonly used for creating ORM (Occlusion, Roughness, Metallic) maps or other channel-packed textures.

## Features

- **4-Channel Packing (RGBA)**: Takes up to four input textures and packs their Red channels into the output's Red, Green, Blue, and Alpha channels respectively.
- **Auto-Resizing**: Automatically resizes input textures to match the specified target resolution using High-Quality Bilinear Interpolation (`FImageUtils`).
- **Input Handling**:
  - Reads the **Red channel** from each source texture.
  - If an input texture is missing, the corresponding channel is filled with Black (0).
  - **Optional Alpha Channel**: If an Alpha texture is assigned, its Red channel is used. If left empty, the Alpha channel defaults to White (255) for full opacity.
- **Extended Format Support**:
  - Supports **16-bit Grayscale** and **32-bit Float (SDF)** source formats, ensuring high-precision data is processed correctly without "black texture" issues.
- **Output Configuration**:
  - **Compression Settings**: Select from `Masks (Recommended)`, `Grayscale`, or `Default` via a dropdown menu.
  - Customizes Output Path, File Name, and Resolution.
  - Generates `UTexture2D` assets with `sRGB = false` (linear color).
- **User Interface**:
  - **Path Picker**: Easily select the output directory from the Content Browser using the folder icon button.
  - **Toast Notifications**: Provides clear feedback (Success/Error) via non-intrusive notifications instead of just log messages.
  - Integrated into the Unreal Engine Editor via the **Tools** menu.

## Why use this tool? (Optimization Benefits)

If you are a Blueprint developer or new to 3D optimization, you might wonder why "Channel Packing" is necessary. Here is why it helps your game performance:

1.  **Avoid Material Errors (Sampler Limits)**
    Unreal Engine Materials have a limit on how many unique texture files they can read (usually 16). If you use separate textures for Ambient Occlusion, Roughness, and Metallic, you use up **3 slots**. By packing them into a single file, you only use **1 slot**, leaving room for more complex effects without hitting the limit.

2.  **Save Graphics Memory (VRAM)**
    Loading one combined texture is more efficient for your graphics card than loading three separate files. This helps your game run smoother and use less video memory.

3.  **Cleaner Project**
    Instead of having three separate files cluttering your Content Browser, you have one clean "ORM" (Occlusion-Roughness-Metallic) texture.

## Requirements

- **Unreal Engine 5.5+** (Developed and tested on 5.7)
- C++ Project (to compile the plugin)

## Installation

1. **Clone or Download**:
   Download this repository and place the `TextureChannelPacker` folder into your project's `Plugins` directory.
   - If the `Plugins` folder does not exist, create it in your project's root directory (next to `.uproject`).

   Directory structure:
   ```
   MyProject/
   ├── MyProject.uproject
   ├── Plugins/
   │   └── TextureChannelPacker/
   │       ├── TextureChannelPacker.uplugin
   │       └── Source/
   └── Source/
   ```

2. **Generate Project Files**:
   Right-click on your `.uproject` file and select **Generate Visual Studio project files**.

3. **Build the Project**:
   Open the generated `.sln` file in your IDE (e.g., Visual Studio or Rider) and build your project. This will compile the plugin.

4. **Enable the Plugin**:
   Launch the Unreal Engine Editor. If the plugin is not enabled automatically, go to **Edit > Plugins**, search for **TextureChannelPacker**, and enable it. Restart the editor if prompted.

## Usage Guide

1. **Open the Tool**:
   In the Unreal Editor, navigate to the main menu bar and select **Tools > Texture Packing**. This will open the Texture Channel Packer tab. You can dock this tab anywhere in your editor layout.

2. **Assign Inputs**:
   - **Red Channel Input**: Select a texture for the Red channel (e.g., Ambient Occlusion).
   - **Green Channel Input**: Select a texture for the Green channel (e.g., Roughness).
   - **Blue Channel Input**: Select a texture for the Blue channel (e.g., Metallic).
   - **Alpha Channel Input** (Optional): Select a texture for the Alpha channel. If empty, it defaults to White (255).

   *Note: You can leave any input empty; R/G/B channels will be filled with black if missing.*

3. **Configure Output**:
   - **Resolution**: Set the target resolution for the output texture (e.g., 2048).
   - **Compression Settings**: Choose the compression type (default is `Masks`).
   - **Output Path**: Specify the game folder path. You can type it manually or click the **Folder Icon** to select a directory from the Content Browser.
   - **File Name**: Enter the desired name for the new texture asset.

4. **Generate**:
   Click the **Generate Texture** button.
   - The tool will process the textures and create a new asset in the Content Browser at the specified location.
   - A Toast Notification will confirm if the operation was successful.

## License

This project is licensed under the MIT License. See the [LICENSE](LICENSE) file for details.
