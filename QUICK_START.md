[日本語 (Japanese)](QUICK_START.ja.md)

# Quick Start Guide

**Zero to Packed Texture in 10 seconds.**

![Tool Overview](Docs/Images/workflow.gif)

## 1. Installation

1.  Copy the `TextureChannelPacker` folder into your project's `Plugins` directory.
2.  Restart Unreal Engine.

## 2. Step-by-Step Workflow

### Step 1: Open the Tool
Go to **Tools > Texture Channel Packer**.

### Step 2: Drag & Drop Inputs
Drag your textures directly from the Content Browser into the slots.
*   *Tip:* Standard ORM layout is **Red** = Ambient Occlusion, **Green** = Roughness, **Blue** = Metallic.

### Step 3: Verify & Generate
1.  Notice that the **Output File Name** is automatically generated (Smart Naming).
2.  Click the **Folder Icon** if you need to change the save location.
3.  Click **Generate Texture**.

## Pro Tips
*   Empty R, G, B slots are automatically filled with **Black (0)**. Alpha defaults to **White (255)**.
*   No need to resize textures beforehand; the tool handles it automatically.
*   For ORM maps, leave the Compression setting on `Masks`.
