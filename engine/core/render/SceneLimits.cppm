// Single source of truth for scene-scale capacity: the block budget, the gather entity cap and the record sizes everything else derives from.
//
// Three things used to be independent magic numbers: the gather-time entity
// cap, the storage-buffer block count, and the 256 entries-per-block literal
// duplicated across the submesh shaders. Nothing tied them together, so raising
// one silently pushed past another. This module names the budget once and
// derives everything else from it:
//
//   kEntriesPerBlock   -- mirror of shaders/scene_block_layout.slang
//   kMaxBlocksCeiling  -- static descriptor-array ceiling
//        |
//        v  clamped by the device at SceneRenderer::Initialize
//   max_blocks_        -- descriptor arrays, descriptor pools, BlockArray
//                         limits, and the gather submesh budget all derive here.
//
// Nothing here describes a GPU resource: the descriptor-set rows, the buffer rows and the block-array rows live in VulkanEngine.SceneGpuLayout, which imports this module for the numbers below.
// These are plain integers on purpose, so a caller that only needs a capacity (gather, mesh upload, bootstrap) does not pull in a descriptor schema it never reads.

module;

export module VulkanEngine.SceneLimits;

import std;
import std.compat;

export namespace VulkanEngine::SceneLimits {

// Entries per storage-buffer block for the per-submesh arrays.
//
// MIRRORED in engine/core/shaders/scene_block_layout.slang. The shaders divide
// a flat submesh index into (block, element) with this value; the CPU side uses
// it for BlockArray geometry. They must be equal.
inline constexpr std::uint32_t kEntriesPerBlock = 256;

// Compile-time ceiling on the number of blocks. Descriptor-set binding array
// sizes are static, so this bounds the layouts; the runtime value chosen at
// Initialize is clamped below this by the device's descriptor limits.
inline constexpr std::uint32_t kMaxBlocksCeiling = 4096;

inline constexpr std::uint32_t kMaxSubmeshesCeiling =
    kMaxBlocksCeiling * kEntriesPerBlock;

// Gather-time entity cap. This is a safety valve against pathological scenes,
// NOT a capacity knob: the real enforcement is the submesh budget derived from
// max_blocks_. Kept far above the block budget so it is never the binding limit.
inline constexpr std::uint32_t kGatherEntityCap = 1u << 20;

// Initial reserve for the per-frame gather vectors. Capacity persists across
// frames (the vectors are cleared, not freed), so this is only the cold-start
// size and never a per-frame allocation of the full ceiling.
inline constexpr std::uint32_t kInitialGatherCapacity = 4096;

// Fixed-size descriptor tables that do not scale with the block budget.
// `kMaxSkinBuffers` mirrors `kMaxVertexBuffers`: the deform pass binds the skin
// influence streams as a fixed descriptor array whose length is independent of
// the vertex heap's runtime block count.
inline constexpr std::uint32_t kMaxVertexBuffers = 64;
inline constexpr std::uint32_t kMaxIndexBuffers = 64;
inline constexpr std::uint32_t kMaxUvBuffers = 64;
inline constexpr std::uint32_t kMaxSkinBuffers = 64;

// Initial capacity of the flat modifier-parameter buffer, in bytes. The buffer
// grows geometrically; this is only the floor, so the first frame that carries a
// modifier does not have to grow (and device-idle) before it can record.
inline constexpr std::uint32_t kInitialModifierParamBytes = 1u << 20;

// ── Lighting set sizes ──
//
// One LightBlock is a fixed 256 lights, mirrored by standard_mesh.slang.
// The block table is a fixed descriptor array, so these bound both the layout and the BlockArray.
inline constexpr std::uint32_t kLightsPerBlock = 256;
inline constexpr std::uint32_t kMaxLightBlocks = 16;

// ── Hi-Z pyramid ──
//
// Storage-image descriptor array size for the hiz set; also the level cap the generator and the cull shaders agree on.
inline constexpr std::uint32_t kMaxHiZMips = 12;

// ── GPU-side record sizes that are engine constants, not types ──
//
// 20 B is sizeof(vk::DrawIndexedIndirectCommand), pinned by a static_assert in SceneRenderer.cpp so the Vulkan header cannot drift away from the layout rows.
inline constexpr std::uint32_t kIndirectCommandBytes = 20;
// 32 B is sizeof(SkinEntry); pinned by the static_assert next to SkinEntry.
inline constexpr std::uint32_t kSkinEntryBytes = 32;

} // namespace VulkanEngine::SceneLimits
