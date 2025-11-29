# TextureChannelPacker

**TextureChannelPacker** is an Unreal Engine 5.7 plugin designed to efficiently pack three separate grayscale textures into the Red, Green, and Blue channels of a single output texture. This is commonly used for creating ORM (Occlusion, Roughness, Metallic) maps or other channel-packed textures.

## Features

- **Channel Packing**: Takes three input textures and packs their Red channels into the output's Red, Green, and Blue channels respectively.
- **Auto-Resizing**: Automatically resizes input textures to match the specified target resolution using High-Quality Bilinear Interpolation (`FImageUtils`).
- **Input Handling**:
  - Reads the **Red channel** from each source texture.
  - If an input texture is missing, the corresponding channel is filled with Black (0).
  - The Alpha channel is always set to 1.0 (White).
- **Output Configuration**:
  - Allows customizing the Output Path, File Name, and Resolution.
  - Generates `UTexture2D` assets with `CompressionSettings = TC_Masks` and `sRGB = false`, optimized for mask data.
- **User Interface**:
  - Integrated into the Unreal Engine Editor via the **Tools** menu.
  - Opens as a dockable **Nomad Tab**.

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
   - **Red Channel Input**: Select a texture to pack into the Red channel (e.g., Ambient Occlusion).
   - **Green Channel Input**: Select a texture to pack into the Green channel (e.g., Roughness).
   - **Blue Channel Input**: Select a texture to pack into the Blue channel (e.g., Metallic).

   *Note: You can leave any input empty; that channel will be filled with black.*

3. **Configure Output**:
   - **Resolution**: Set the target resolution for the output texture (e.g., 2048). All inputs will be resized to this resolution.
   - **Output Path**: Specify the game folder path where the asset will be saved (e.g., `/Game/Textures/Packed`).
   - **File Name**: Enter the desired name for the new texture asset.

4. **Generate**:
   Click the **Generate Texture** button.
   - The tool will process the textures and create a new asset in the Content Browser at the specified location.
   - The output texture will have sRGB disabled and Compression Settings set to Masks (Linear Color).

## License

This project is licensed under the MIT License. See the [LICENSE](LICENSE) file for details.
