## 2024-05-22 - [Redundant Texture Array Allocations]
**Learning:** The `TextureChannelPacker` processes textures by converting everything to `TArray<FColor>` even when data could be processed directly (e.g., Grayscale inputs). This causes massive memory spikes (3x-4x overhead) for large textures.
**Action:** When processing texture data in UE, always check if the source format and resolution match the target to allow direct memory operations (Memcpy/Extraction) instead of generic conversion pipelines.
