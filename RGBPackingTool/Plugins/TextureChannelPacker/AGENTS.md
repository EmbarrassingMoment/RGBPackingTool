# Texture Channel Packer — notes for AI coding agents

This file is for an AI agent (Codex, Cursor, GitHub Copilot, Gemini CLI, Jules, Claude Code, …) working in an
Unreal Engine project that has the **Texture Channel Packer** plugin installed. It is vendor-neutral: `CLAUDE.md`
next to it only imports this file. If your tool does not pick this file up on its own, point it here (for example
with one line in your project's own `AGENTS.md` / `CLAUDE.md`).

If this folder sits inside a checkout of the plugin's own repository (RGBPackingTool, with `Source/` and a sibling
`AGENTS.md` one level up in `RGBPackingTool/`), you are developing the plugin itself: follow that repository-level
file instead, and ignore the "Do not" section below, which is written for installed copies.

## What the plugin does

Editor-only plugin for Unreal Engine 5.5+ that **packs** up to four grayscale textures into the R/G/B/A channels
of one texture (ORM / MRA style masks) and **unpacks** a packed texture back into per-channel grayscale
textures. Everything the tool window does can also run **headless**, without dialogs, from a commandlet,
Editor Python, Editor Utility Blueprints or Remote Control.

## Read this first

**`Docs/Headless.md`** (Japanese: `Docs/Headless.ja.md`) in this folder is the complete and authoritative
reference: job file schema, defaults, Python API, result file, behavior notes and the error code table.
Do not guess field names or defaults; read the doc. It is checked against the C++ source in CI.

## How to drive the plugin

- **Batch work (preferred)** — write a JSON job file and run the commandlet from the project folder:

  ```
  UnrealEditor-Cmd.exe <Project>.uproject -run=TextureChannelPacker -Job=jobs.json -Result=result.json -unattended -nullrhi -nosplash
  ```

  Exit code `0` = every job succeeded (skipped and dry-run jobs count as success), `1` = bad arguments or an
  unreadable job file, `2` = at least one job failed. Starting the editor is the expensive part, so put many
  jobs in one file; jobs run in order and later jobs may use assets earlier jobs created.
- **Inside a running editor** — use Editor Python (`unreal.TextureChannelPackerLibrary`) or Remote Control;
  the request/result fields mirror the job file (see the Python section of `Docs/Headless.md`).
- **Check before writing** — add `-DryRun` (or `"dryRun": true` on a job) to validate inputs and resolve the
  output names and size without touching any asset.

## Rules of thumb

- Read `result.json` and branch on each entry's `status` and `errorCode`. `errorCode` is stable and
  locale-independent; `message` follows the editor language and may be Japanese.
- Unknown job fields are rejected, not ignored, so a typo fails loudly. Fix the field name instead of retrying.
- Overwriting is opt-in. The default policy fails when an output asset exists; use `"overwrite": "skip"` to make a
  job file safe to re-run, or `"overwrite": "overwrite"` when replacing is intended.
- Outputs are saved to disk by default. Under source control the files must be writable (checked out) first.
- Headless runs need an **editor** build and the textures' source data; they are not for runtime (in-game) use.
- Asset paths are content paths such as `/Game/Folder/T_Name`, not file system paths.

## Do not

- Do not modify files inside this plugin folder (source, `.uplugin`, docs). It is an installed third-party plugin;
  changes are lost on update. Report issues at https://github.com/EmbarrassingMoment/RGBPackingTool instead.
- Do not read texture pixels yourself to "help" the tool; feed it the asset paths and let it resize and pack.
