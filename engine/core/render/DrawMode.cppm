module;

export module VulkanEngine.DrawMode;

import std;

export namespace VulkanEngine::SceneRenderer {

// Indexed-drawing compaction / draw shape (docs/indexed-drawing-pipeline.md).
// Mode names follow the [qualifier]ID convention: the qualifier names the
// compaction output and ID means Indirect Draw. CID is Compacted-Indirect Draw;
// MID is Multi-Indirect Draw. A new mode takes a new qualifier and the same
// suffix.
//
// Fixed at renderer initialization; changing it re-creates the mode-dependent
// frame buffers and rebuilds the kCompactionMode-specialized compute pipelines.
enum class DrawMode : std::uint8_t {
    CID = 0,   // 4 B absolute slot indices, one drawIndexedIndirect per pass/technique
    MID = 1,   // 20 B DrawIndexedIndirectCommand per alive submesh, drawIndexedIndirectCount
};

} // namespace VulkanEngine::SceneRenderer
