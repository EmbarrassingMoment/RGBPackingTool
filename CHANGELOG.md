[日本語 (Japanese)](CHANGELOG.ja.md)

# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
