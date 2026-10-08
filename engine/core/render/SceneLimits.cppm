// Single source of truth for scene-scale capacity.
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

module;

export module VulkanEngine.SceneLimits;

import std;

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
inline constexpr std::uint32_t kMaxVertexBuffers = 64;
inline constexpr std::uint32_t kMaxIndexBuffers = 64;
inline constexpr std::uint32_t kMaxUvBuffers = 64;

// ── Descriptor-set plans ──
//
// One row per SceneRenderer-owned descriptor set. Layout creation and
// descriptor-pool sizing both read the same row, so a block-array size can no
// longer be declared in one place and budgeted in another.
//
// `block_bindings` counts bindings whose descriptorCount is the runtime block
// count; `single_storage_buffers` counts fixed-count storage-buffer bindings in
// the same set.
struct SetPlan {
    std::string_view name;
    std::uint32_t block_bindings = 0;
    std::uint32_t single_storage_buffers = 0;
    std::uint32_t combined_image_samplers = 0;
    std::uint32_t sampled_images = 0;
};

inline constexpr SetPlan kSubmeshVertexPlan{"submesh-vertex", 1, 0, 0, 0};
// Set 4: expand -- 4 block arrays + vertex_indirection, draw_indices,
// expand_counter.
inline constexpr SetPlan kExpandPlan{"expand", 4, 3, 0, 0};
// Set 5: occlusion -- submesh vertex/cull/sphere/obb blocks (4) plus the
// pre-cull survivor bindings (4) and one Hi-Z combined image sampler.
inline constexpr SetPlan kOcclusionPlan{"occlusion", 4, 4, 1, 1};
// Set 8: occluder select -- cull/vertex/obb blocks (3) + 5 singles.
inline constexpr SetPlan kOccluderSelectPlan{"occluder-select", 3, 5, 0, 0};
// Set 6: collect -- cull blocks (1) + 5 singles.
inline constexpr SetPlan kCollectPlan{"collect", 1, 5, 0, 0};
// The depth/main graphics pipelines bind submesh-vertex (blocks) together with
// the vertex (+UV) and index/implicit tables on one stage. This row models that
// per-stage sum so the per-stage limit is checked too.
inline constexpr SetPlan kDepthGraphicsStagePlan{
    "depth-graphics-stage", 1, kMaxVertexBuffers + kMaxUvBuffers, 0, 0};

inline constexpr std::array<SetPlan, 6> kSceneSetPlans{{
    kSubmeshVertexPlan,
    kExpandPlan,
    kOcclusionPlan,
    kOccluderSelectPlan,
    kCollectPlan,
    kDepthGraphicsStagePlan,
}};

// Storage buffers in one descriptor set for a plan at `blocks` blocks.
[[nodiscard]] constexpr std::uint32_t
StorageBuffersForPlan(const SetPlan& plan, const std::uint32_t blocks) noexcept {
    return plan.block_bindings * blocks + plan.single_storage_buffers;
}

// Largest single-set storage-buffer count across all plans at `blocks` blocks.
[[nodiscard]] constexpr std::uint32_t
MaxStorageBuffersPerSet(const std::uint32_t blocks) noexcept {
    std::uint32_t max_buffers = 0;
    for (const SetPlan& plan : kSceneSetPlans) {
        max_buffers = std::max(max_buffers, StorageBuffersForPlan(plan, blocks));
    }
    return max_buffers;
}

// Descriptor limits the block budget depends on, kept free of Vulkan types so
// the derivation is unit-testable without a device.
struct DeviceLimits {
    std::uint32_t max_descriptor_set_storage_buffers = 0;
    std::uint32_t max_per_stage_descriptor_storage_buffers = 0;
};

// Largest block count whose worst-case set fits every limit. Never returns 0:
// a device that cannot fit even one block still gets one, and the caller
// reports the condition rather than dividing by zero.
[[nodiscard]] constexpr std::uint32_t
MaxBlocksForDevice(const DeviceLimits& limits) noexcept {
    std::uint32_t best = kMaxBlocksCeiling;
    for (const SetPlan& plan : kSceneSetPlans) {
        if (plan.block_bindings == 0) {
            continue;
        }
        for (const std::uint32_t limit :
             {limits.max_descriptor_set_storage_buffers,
              limits.max_per_stage_descriptor_storage_buffers}) {
            if (limit <= plan.single_storage_buffers) {
                // Even zero blocks would not fit this binding's fixed part.
                best = 0;
                continue;
            }
            const std::uint32_t fit =
                (limit - plan.single_storage_buffers) / plan.block_bindings;
            best = std::min(best, fit);
        }
    }
    return std::max<std::uint32_t>(best, 1u);
}

} // namespace VulkanEngine::SceneLimits
