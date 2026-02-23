[日本語 (Japanese)](CHANGELOG.ja.md)

# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.3.0] - 2026-02-23

### Added
- **Invert Toggle**: Added a per-channel Invert checkbox next to each input slot label. When enabled, the channel values are flipped (`255 - Value`), useful for conversions like Roughness to Smoothness.
- **Non-Square Output**: Width and Height can now be specified independently, replacing the single Resolution field. This enables non-square packed textures for use cases like UI atlases.
- **Overwrite Confirmation**: A confirmation dialog now appears when the output asset already exists, preventing accidental data loss.

### Changed
- **Compression Settings Refactor**: Migrated internal compression option handling from string-based comparison to an enum/struct-based architecture (`FCompressionOption`), improving maintainability and localization safety.

## [1.2.0] - 2026-01-25

### Fixed
- **Memory Leak**: Addressed an issue where `UPackage` objects were not properly garbage collected when texture generation was cancelled or failed. Implemented RAII using `TStrongObjectPtr` and proper cleanup logic.

## [1.1.0] - 2026-01-16

### Added
- **Parallel Processing**: Implemented multi-threaded processing for texture channel resizing and conversion using `ParallelFor`, improving generation speed.
- **Cancellable Progress**: Added a progress dialog with a cancel button, allowing users to abort the texture generation process.
- **Extended Format Support**: Added support for `TSF_RGBA32F` (Linear Color) input textures.

## [1.0.1] - 2025-12-09

### Fixed
- **Smart Naming**: Fixed an issue where auto-generation would overwrite manually edited filenames. It now respects user input.
- **Color Space**: Forced `SRGB = false` on generated textures to ensure correct linear values for packed channels.
- **Packaging**: Added `PlatformAllowList` to `.uplugin` to prevent packaging errors on mobile platforms.

### Changed
- **Fab Preparation**: Updated `.uplugin` metadata (EngineVersion, Installed) for Fab store submission.
- **License**: Updated Copyright year to 2026.

## [1.0.0] - 2025-12-08

### Added
- **Core Features**
  - RGBA Channel Packing (Supports Red, Green, Blue, and optional Alpha inputs).
  - Auto-Resizing of input textures to match target resolution using high-quality bilinear interpolation.
  - Smart File Naming: Automatically generates output filenames based on input texture names.
- **UI & UX**
  - Path Picker button for easy output directory selection.
  - Compression Settings dropdown (Masks, Grayscale, Default).
  - Toast Notifications for success/error feedback.
  - Drag & Drop support for texture slots.
  - Full UI Localization (English/Japanese) based on Editor language.
- **Technical Improvements**
  - Extended Format Support: Handles 8-bit, 16-bit Grayscale, and 16/32-bit Float (SDF) textures.
  - Memory Optimization: Reduced peak memory usage during packing of large (e.g., 8K) textures.
  - Performance: Implemented lazy allocation for pixel data processing.
  - Validation: Enforced resolution limits (1-8192) and input checks.
