[日本語 (Japanese)](Headless.ja.md)

# Headless Usage (Commandlet / Python / Blueprint)

Pack and Unpack can run without the tool window, so they can be driven from scripts, CI, and AI agents. Every entry point runs the same pipeline as the editor tool (`TextureChannelPackerCore`), but **never opens a dialog or shows a notification**: everything is reported in a structured result with a stable error code.

| Entry point | Use it when |
| :--- | :--- |
| **Commandlet** (`-run=TextureChannelPacker`) | Batch jobs from CI or an agent: JSON job file in, JSON result file out, exit code. |
| **Python** (`unreal.TextureChannelPackerLibrary`) | Editor Python scripts, or tools that drive a running editor through Python. |
| **Blueprint** (Editor Utility) | Editor Utility Widgets / Blueprints. |
| **Remote Control** | The library's functions are `BlueprintCallable` statics, so a running editor can also be reached through the Remote Control API (call them on the class default object, `/Script/TextureChannelPacker.Default__TextureChannelPackerLibrary`). |

Headless runs are **Editor-only** (they need the textures' source data), and must run on the Game Thread (all of the entry points above do).

## Commandlet

```
UnrealEditor-Cmd.exe <Project>.uproject -run=TextureChannelPacker -Job=jobs.json -Result=result.json -unattended -nullrhi -nosplash
```

| Argument | Description |
| :--- | :--- |
| `-Job=<path>` | Required. The job file (see below). |
| `-Result=<path>` | Where to write the result JSON. If omitted, the result is printed to the log. |
| `-DryRun` | Forces `dryRun` on every job: validate and resolve names without writing anything. |

Relative paths are resolved against the directory the command was launched from.

**Exit codes**: `0` every job succeeded (`Skipped` and `DryRun` count as success) · `1` bad arguments or unreadable job file · `2` at least one job failed.

Jobs run in order, so a later job can use an asset an earlier job created. Starting the editor is the expensive part, so put many jobs in one file rather than launching once per texture.

### Job file

```json
{
  "jobs": [
    {
      "mode": "pack",
      "preset": "ORM",
      "inputs": {
        "R": "/Game/Rock/T_Rock_AO",
        "G": "/Game/Rock/T_Rock_Roughness",
        "B": { "texture": "/Game/Rock/T_Rock_Metallic", "channel": "R", "invert": false }
      },
      "outputPath": "/Game/Rock",
      "overwrite": "overwrite"
    },
    {
      "mode": "unpack",
      "preset": "ORM",
      "source": "/Game/Props/T_Crate_ORM",
      "outputPath": "/Game/Props/Split"
    }
  ]
}
```

Unknown fields are rejected (`ErrorInvalidJob`), so a typo fails loudly instead of being ignored. Asset paths may be written as `/Game/Folder/T_Name`, `/Game/Folder/T_Name.T_Name`, or `Texture2D'/Game/Folder/T_Name.T_Name'`.

#### Pack job

| Field | Default | Description |
| :--- | :--- | :--- |
| `mode` | — | `"pack"` |
| `preset` | — | Built-in (`Custom`, `ORM`, `MRA`) or user preset name. Supplies source channels, invert flags, compression, and filename suffix; explicit fields override it. |
| `inputs` | — | Required. Object with keys `R`/`G`/`B`/`A`. Each value is an asset path, or `{ "texture", "channel": "R\|G\|B\|A", "invert": bool }`. Missing slots are filled with 0 (R/G/B) or 255 (A). |
| `width`, `height` | `0`, `0` | Output size (1–16384). Both `0` = the size of the largest input. |
| `outputPath` | `"/Game"` | Content folder for the output. |
| `outputName` | auto | Output asset name. Omitted = the tool's auto naming (common prefix of the inputs + `suffix`, `T_` enforced). |
| `suffix` | preset / `"_ORM"` | Suffix used by auto naming. |
| `compression` | preset / `"Masks"` | `"Masks"`, `"Grayscale"`, or `"Default"`. |
| `overwrite` | `"fail"` | `"fail"`, `"overwrite"`, or `"skip"` (see below). |
| `save` | `true` | Save the package to disk. |
| `dryRun` | `false` | Validate and resolve the name and size without reading pixels or writing anything. |

#### Unpack job

| Field | Default | Description |
| :--- | :--- | :--- |
| `mode` | — | `"unpack"` |
| `preset` | — | Supplies the per-channel suffixes (ORM: `_AO` / `_Roughness` / `_Metallic` / `_A`). |
| `source` | — | Required. The packed texture. |
| `channels` | `"auto"` | `"auto"`: every channel except uniform ones (all pixels share one value, e.g. an unused alpha). `"all"`: every channel. `["R", "G"]`: exactly those channels. |
| `outputPath` | `"/Game"` | Content folder for the outputs. |
| `baseName` | auto | Base name; the channel suffix is appended. Omitted = derived from the source name (`T_Rock_ORM` → `T_Rock`). |
| `suffixes` | preset / `_R` `_G` `_B` `_A` | Object with keys `R`/`G`/`B`/`A`. |
| `overwrite`, `save`, `dryRun` | | As for pack. For unpack, `"skip"` writes nothing if *any* output already exists. |

### Result file

```json
{
  "version": 1,
  "total": 2,
  "failed": 0,
  "results": [
    {
      "index": 0,
      "mode": "pack",
      "status": "Succeeded",
      "errorCode": "",
      "message": "Texture saved: /Game/Rock/T_Rock_ORM",
      "outputs": [ "/Game/Rock/T_Rock_ORM.T_Rock_ORM" ],
      "warnings": [],
      "width": 2048,
      "height": 2048
    },
    {
      "index": 1,
      "mode": "unpack",
      "status": "Succeeded",
      "errorCode": "",
      "message": "Unpacked 3 texture(s) to /Game/Props/Split",
      "outputs": [ "/Game/Props/Split/T_Crate_AO.T_Crate_AO", "..." ],
      "warnings": [],
      "width": 1024,
      "height": 1024,
      "skippedUniformChannels": [ "A" ]
    }
  ]
}
```

- `status`: `Succeeded`, `Skipped`, `DryRun`, or `Failed`.
- `errorCode`: stable and locale-independent; branch on this, not on `message` (which follows the editor language).
- `outputs`: object paths written, or, for `DryRun` / `Skipped`, the paths the job targets.
- `warnings`: non-fatal problems, each `{ "code", "channel", "message" }`. For pack, a warning means an input could not be read and its slot was filled with the default value.
- If the run cannot start (bad arguments, unreadable job file), the result contains `"error": { "code", "message" }` and an empty `results` array.

## Python

```python
import unreal

lib = unreal.TextureChannelPackerLibrary

red = unreal.ChannelPackerInput()
red.texture = unreal.load_asset("/Game/Rock/T_Rock_AO")
green = unreal.ChannelPackerInput()
green.texture = unreal.load_asset("/Game/Rock/T_Rock_Roughness")

request = unreal.ChannelPackerPackRequest()
request.red = red
request.green = green
request.output_path = "/Game/Rock"
request.overwrite_policy = unreal.ChannelPackerOverwritePolicy.OVERWRITE

result = lib.pack_textures(request)
if result.status == unreal.ChannelPackerStatus.FAILED:
    unreal.log_error(f"{result.error_code}: {result.message}")
else:
    print(result.output_assets)
```

| Function | Description |
| :--- | :--- |
| `pack_textures(request)` | Runs a `ChannelPackerPackRequest`; returns a `ChannelPackerResult`. |
| `unpack_texture(request)` | Runs a `ChannelPackerUnpackRequest`; returns a `ChannelPackerResult`. |
| `make_pack_request_from_preset(name)` / `make_unpack_request_from_preset(name)` | Fills a request with a preset's defaults; reports `False` if no preset has that name. |
| `get_preset_names()` | Built-in and user preset names. |

The request fields mirror the job file (`width`, `height`, `output_path`, `output_name`, `file_name_suffix`, `compression`, `overwrite_policy`, `save`, `dry_run`, and for unpack `source`, `export_red`… `export_alpha`, `skip_uniform_channels`, `base_name`, `suffix_red`… `suffix_alpha`). Unlike the editor tool, `save` defaults to `True`.

## Behavior Notes

- **Overwrite policy**: `Fail` (default) fails with `ErrorAssetExists`; `Overwrite` replaces the asset in place (as the editor tool does after its confirmation dialog); `Skip` writes nothing and reports `Skipped`, which makes re-running a job file safe. An existing asset of another class is never replaced (`ErrorAssetTypeMismatch`).
- **Saving**: With `save`, the texture build is finished and the package is written with `UPackage::SavePackage`. Files under source control must be writable (checked out) beforehand, otherwise the job fails with `ErrorSaveFailed`.
- **Output format**: the same as the editor tool. Pack writes BGRA8 with the chosen compression; Unpack writes G8 with `Grayscale` compression. Both are linear (`sRGB = false`).

## Error Codes

| Code | Meaning |
| :--- | :--- |
| `ErrorNoTextures` | Pack: no input assigned. |
| `ErrorInputNotFound` | An input/source path does not resolve to a `Texture2D`. |
| `ErrorInvalidResolution` | Width/height outside 1–16384, or only one of them is 0. |
| `ErrorInvalidOutputName` / `ErrorInvalidOutputPath` | The output name or folder would not make a valid asset (e.g. spaces, or a path not under a content root such as `/Game`). |
| `ErrorInvalidCompression` | Unknown compression name. |
| `ErrorAssetExists` | An output exists and the overwrite policy is `Fail`. |
| `ErrorAssetTypeMismatch` | An object of another class already uses the output name. |
| `ErrorSaveFailed` | The package could not be saved. |
| `ErrorNoUnpackSource`, `ErrorNoChannelsSelected` | Unpack: no source, or no channel requested. |
| `ErrorNoChannelsToExport` | Unpack: every requested channel is uniform (use `"channels": "all"` to export anyway). |
| `ErrorDuplicateOutputNames` | Unpack: two channels resolve to the same name (check the suffixes). |
| `ErrorUnsupportedFormat`, `ErrorLockFailed`, `ErrorTextureTooLarge`, `ErrorInvalidSourceData`, `ErrorInvalidUnpackSource` | The texture's source data cannot be read. For pack inputs these are warnings (the slot gets its default value). |
| `ErrorInvalidJob` | Commandlet: the job entry does not match the schema (the message says which field). |
| `ErrorUsage`, `ErrorJobFileNotFound`, `ErrorInvalidJobFile` | Commandlet: the run could not start. |
