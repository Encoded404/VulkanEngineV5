module;
#include <glm/glm.hpp> // NOLINT(misc-include-cleaner)

export module VulkanEngine.SceneRenderer;

import std;
import std.compat;

import vulkan;

export import VulkanEngine.ECS.ComponentRegistry;
export import VulkanBackend.Vulkan.VulkanBootstrap;
export import VulkanShared.CallbackList;
export import VulkanEngine.Components.Transform;
export import VulkanEngine.Components.MeshReference;
export import VulkanEngine.Components.Text;
export import VulkanEngine.StandardMeshPipeline;
export import VulkanEngine.TechniqueManager;
export import VulkanEngine.BindlessManager;
export import VulkanEngine.Mesh.MeshTypes;
export import VulkanEngine.GpuResources;
export import VulkanEngine.GpuResources.BlockArray;
export import VulkanEngine.SceneLimits;
export import VulkanEngine.DrawMode;
export import VulkanEngine.ModifierId;
import VulkanEngine.ModifierManager;
import VulkanEngine.PipelineFactory;
import VulkanEngine.ShaderManager;
import VulkanEngine.ShaderRegistration;

export namespace VulkanEngine::SceneRenderer {

// NOLINTBEGIN(modernize-avoid-c-arrays)
// ── Scene uniform data (replaces push-constant lighting) ──
//
// Contract mirror for the shader-side Light (engine/core/shaders/
// standard_mesh.slang). The light TYPE dispatch lives in position.w:
//   0 = directional: L = -direction.xyz, no attenuation.
//   1 = point:       L from position.xyz; inverse-square falloff windowed by
//                    params.x range (<= 0 = infinite).
//   2 = spot:        as point; direction.xyz is the cone axis pointing AWAY
//                    from the light, params.y/z = cos(inner)/cos(outer).
// color = rgb tint + w intensity. params is otherwise unused.
struct alignas(16) Light {
    float position[4];
    float color[4];
    float direction[4];
    float params[4];
};

struct alignas(16) SceneHeader {
    float ambient_color[4];
    float sun_direction[4];
    float sun_color[4];
    std::uint32_t light_count = 0;
};
// NOLINTEND(modernize-avoid-c-arrays)

constexpr std::uint32_t LIGHTS_PER_BLOCK = 256;
constexpr std::uint32_t MAX_LIGHT_BLOCKS = 16;

// ── Deform addressing contract (mirrored in shaders/skin_layout.slang) ──
//
// One `SkinEntry` per submesh tells the deform stage where that submesh's skin
// influences live, where its bone palette starts, and which two contiguous
// regions of the deformed-vertex buffer it may ping-pong between. It is written
// by gather before the frame records and, for `deform_base`, by expand on the
// GPU (the only place that owns the per-frame region allocator).
inline constexpr std::uint32_t kNoSkinSlot = 0xFFFFFFFFu;
// `VertexEntry::deformBias` sentinel: the vertex is read from the source table,
// not from a deformed region. Also pinned in shaders/scene_entries.slang.
inline constexpr std::int32_t kNoDeform = std::numeric_limits<std::int32_t>::min();

inline constexpr std::uint32_t kDeformActive = 1u << 0;  // expand allocated a region
inline constexpr std::uint32_t kDeformNoOccluder = 1u << 1;
inline constexpr std::uint32_t kDeformNoCullTest = 1u << 2;
inline constexpr std::uint32_t kSkinned = 1u << 3;       // skin tables are valid
inline constexpr std::uint32_t kFinalRegionB = 1u << 4;  // stack parity

// CullEntry.flags bit 0: selected as an occluder this frame (technique_packing.slang).
inline constexpr std::uint32_t kOccluderBit = 1u << 0;

// CullEntry.flags bits expand copies out of a SkinEntry (technique_packing.slang
// declares the same two). An object with an active deform has no trustworthy
// rest-pose bounds: occluder_select skips it and pre_cull/occlusion_cull keep it
// alive without testing stale bounds.
inline constexpr std::uint32_t kCullNoOccluder = 1u << 1;
inline constexpr std::uint32_t kCullNoCullTest = 1u << 2;

struct SkinEntry {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::uint32_t palette_base = 0;    // global matrix index into bone_palettes
    std::uint32_t skin_slot = 0;       // slot in the skin-influence table
    std::int32_t skin_bias = 0;        // skin_region_base - source base element
    std::uint32_t extra_slot = 0;      // kNoSkinSlot when the mesh has one set
    std::int32_t extra_bias = 0;
    std::uint32_t influence_sets = 0;  // 0/1/2
    std::uint32_t flags = 0;           // kDeform*/kSkinned/kFinalRegionB
    std::uint32_t deform_base = 0;     // written by expand, read by the deform pass
    // NOLINTEND(misc-non-private-member-variables-in-classes)
};
static_assert(sizeof(SkinEntry) == 32, "SkinEntry must match the Slang mirror (32 B)");

// One compute dispatch in a submesh's modifier stack, in execution order.
// CPU-only: it is never uploaded, so it carries no GPU layout constraints.
struct ModifierDispatch {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::uint32_t submesh_index = 0;  // global renderer submesh index
    std::uint32_t stage = 0;          // 0-based position in the effective stack
    Modifiers::ModifierId modifier{};
    std::uint32_t vertex_count = 0;   // submesh tight-window span
    std::uint32_t src_slot = 0;       // vertex/skin table slot for stage 0
    std::uint32_t src_base = 0;       // absolute source element index
    std::uint32_t param_offset = 0;   // byte offset into modifier_params
    std::uint32_t param_size = 0;
    std::uint32_t workgroup_size = 64;
    // NOLINTEND(misc-non-private-member-variables-in-classes)
};

// Push constants of every modifier dispatch (32 B, all 32-bit words).
struct ModifierPC {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::uint32_t submesh_index;
    std::uint32_t stage;
    std::uint32_t src_slot;
    std::uint32_t src_base;
    std::uint32_t vertex_count;
    std::uint32_t param_offset;
    std::uint32_t param_size;
    std::uint32_t region_span;  // T: elements per deform region, not 2T
    // NOLINTEND(misc-non-private-member-variables-in-classes)
};
static_assert(sizeof(ModifierPC) == 32, "ModifierPC must match the shader push constant");

// Stable map key for a modifier handle: the slot value and its generation.
// A stale handle can never alias the entry that recycled its slot.
[[nodiscard]] constexpr std::uint64_t ModifierKey(const Modifiers::ModifierId id) noexcept {
    return (static_cast<std::uint64_t>(id.generation) << 32) |
           static_cast<std::uint64_t>(id.value);
}

// ── GPU entry mirrors ──
//
// Byte-identical C++ mirrors of the per-submesh records in
// shaders/scene_entries.slang (scalar block layout: matrices are 4-byte
// aligned, float3x3 is three 12-byte rows). They live here because the cull
// and deform passes write them and the engine's diagnostics/tests seed them;
// the shaders' static layouts are pinned by the static_asserts below.
struct VertexEntry {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::array<float, 16> mvp{};           // 0
    float max_scale = 0.0f;                // 64
    std::uint32_t material_id = 0;         // 68
    std::uint32_t orm_packed = 0;          // 72
    std::array<float, 16> model_matrix{};  // 76
    std::array<float, 9> normal_matrix{};  // 140 (row-major, 12B row stride)
    std::uint32_t vertex_buffer_slot = 0;  // 176
    std::uint32_t uv_slot = 0;             // 180
    std::int32_t uv_bias = 0;              // 184
    std::int32_t deform_bias = 0;          // 188 (kNoDeform when not deformed)
    // NOLINTEND(misc-non-private-member-variables-in-classes)
};
static_assert(sizeof(VertexEntry) == 192, "VertexEntry must match scene_entries.slang");

struct CullEntry {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::uint32_t index_base = 0;
    std::uint32_t index_count = 0;
    std::uint32_t technique_id = 0;
    std::uint32_t flags = 0;  // kOccluderBit / kCull* bits
    // NOLINTEND(misc-non-private-member-variables-in-classes)
};
static_assert(sizeof(CullEntry) == 16, "CullEntry must match scene_entries.slang");

struct BoundingSphereEntry {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float radius = 0.0f;
    // NOLINTEND(misc-non-private-member-variables-in-classes)
};
static_assert(sizeof(BoundingSphereEntry) == 16,
              "BoundingSphereEntry must match the bounding_spheres block layout");

struct ObbEntry {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    float cx = 0.0f;
    float cy = 0.0f;
    float cz = 0.0f;
    float pad0 = 0.0f;
    float ux = 0.0f;
    float uy = 0.0f;
    float uz = 0.0f;
    float hu = 0.0f;
    float vx = 0.0f;
    float vy = 0.0f;
    float vz = 0.0f;
    float hv = 0.0f;
    float wx = 0.0f;
    float wy = 0.0f;
    float wz = 0.0f;
    float hw = 0.0f;
    // NOLINTEND(misc-non-private-member-variables-in-classes)
};
static_assert(sizeof(ObbEntry) == 64, "ObbEntry must match scene_entries.slang");

// ── Deform addressing helpers ──
//
// The addressing convention has exactly one definition here, used by gather, the
// tests and (by construction) the shader mirror, instead of being re-derived as
// inline shifts in every caller. All arguments are element indices, never byte
// offsets.

// Re-bases an absolute source element index into a table whose region starts at
// `region_base`: `table[DeformBias(region_base, source_base) + source_index]`
// names the same element. Signed because a region may start below the source
// base (the second block of a heap), and a negative bias is legitimate.
[[nodiscard]] constexpr std::int32_t DeformBias(const std::uint32_t region_base,
                                               const std::uint32_t source_base) noexcept {
    return static_cast<std::int32_t>(region_base) - static_cast<std::int32_t>(source_base);
}

// Base element of the region that stage `stage` writes: even stages write A
// (`deform_base`), odd stages B (`deform_base + region_span`).
[[nodiscard]] constexpr std::uint32_t DeformStageRegion(const std::uint32_t deform_base,
                                                        const std::uint32_t region_span,
                                                        const std::uint32_t stage) noexcept {
    return (stage & 1u) != 0u ? deform_base + region_span : deform_base;
}

// Base element of the region the vertex shader reads after `stage_count` stages:
// an even stack ends in B, an odd stack in A. A zero stage count means no deform,
// and the result is unused.
[[nodiscard]] constexpr std::uint32_t DeformFinalRegion(const std::uint32_t deform_base,
                                                        const std::uint32_t region_span,
                                                        const std::uint32_t stage_count) noexcept {
    return (stage_count & 1u) == 0u ? deform_base + region_span : deform_base;
}

// Minimum alignment of a modifier parameter block.
inline constexpr std::uint32_t kDeformParamMinAlignment = 16;

// Start offset of a modifier parameter block at `cursor`. `alignment` must be a
// power of two; ModifierManager rejects anything else, so the round-up is exact.
[[nodiscard]] constexpr std::uint32_t DeformParamOffset(
    const std::uint32_t cursor, const std::uint32_t alignment) noexcept {
    const std::uint32_t align = alignment > kDeformParamMinAlignment
        ? alignment
        : kDeformParamMinAlignment;
    return (cursor + align - 1u) & ~(align - 1u);
}

// StaticEntry packing: the high 8 bits carry the heap table slot, the low 24 the
// element offset. The vertex path requires base + windowBase + span < 2^24; the
// index path relies on the same layout but does not assert it.
inline constexpr std::uint32_t kEntrySlotShift = 24;
inline constexpr std::uint32_t kEntryElementMask = (1u << 24) - 1u;

[[nodiscard]] constexpr std::uint32_t PackEntry(const std::uint32_t slot,
                                                const std::uint32_t element) noexcept {
    return (slot << kEntrySlotShift) | element;
}

[[nodiscard]] constexpr std::uint32_t EntrySlot(const std::uint32_t packed) noexcept {
    return packed >> kEntrySlotShift;
}

[[nodiscard]] constexpr std::uint32_t EntryElement(const std::uint32_t packed) noexcept {
    return packed & kEntryElementMask;
}

// ── GPU technique flag-table bit layout ──
// SceneRenderer::UpdateTechniqueFlags packs BaseTechnique::PipelineFlags into a
// uint[MAX_DRAW_GROUPS] storage buffer consumed by the occluder-select,
// pre-cull and occlusion cull shaders. Bit positions must match the kFlag*
// constants in occluder_select.slang, pre_cull.slang and occlusion_cull.slang.
inline constexpr std::uint32_t TECHNIQUE_FLAG_DEPTH_PASS = 1u << 0;           // participates_in_depth_pass
inline constexpr std::uint32_t TECHNIQUE_FLAG_RECEIVES_OCCLUSION = 1u << 1;   // receives_occlusion
inline constexpr std::uint32_t TECHNIQUE_FLAG_COLLECT = 1u << 2;              // participates_in_collect
inline constexpr std::uint32_t TECHNIQUE_FLAG_OCCLUDER_SAFE = 1u << 3;        // bounds_conservative (§5.5)

// Occluder selection: coverage threshold as a fraction of screen area and the
// entry budget (docs/pre-prepass-occlusion-culling.md §5.1-A).
inline constexpr float kOccluderMinAreaFraction = 0.0025f; // 0.25 % of the screen

// DrawMode (CID / MID) is defined by VulkanEngine.DrawMode and re-exported above.

// Runtime scene totals that size the per-frame indexed-drawing buffers. Since
// MeshRenderSystem::ProcessFrame accumulates the real values after upload,
// EnsureSceneCapacity grows the ring geometrically rather than relying on the
// initial (init-time) estimate.
struct SceneCapacity {
    std::uint32_t index_count = 0;   // Σ submesh index_count over the frame
    std::uint32_t vertex_span = 0;   // Σ submesh tight vertex-window span
    std::uint32_t submesh_count = 0; // Σ submeshes (alive + culled) over the frame
    // Σ tight-window span of the submeshes that run a deform stack. The deformed
    // vertex buffer holds two of these regions per submesh (A and B), so its
    // element count is 2 * deform_vertex_span.
    std::uint32_t deform_vertex_span = 0;
    // Σ joint count of the active skins, i.e. the number of mat4 entries in the
    // flat bone-palette buffer.
    std::uint32_t palette_matrix_count = 0;
    // Aligned modifier-parameter bytes for the frame.
    std::uint32_t modifier_param_bytes = 0;
};

class SceneRenderer {
public:
    static constexpr std::uint32_t MAX_HIZ_MIPS = 12;
    // DispatchHiZGen emits up to this many levels per iteration (base and
    // base+1). The generator and the initialization-time coverage assert share
    // this value so the two cannot drift apart.
    static constexpr std::uint32_t HIZ_BATCH = 2;
    // Fixed descriptor-table sizes. Single source: SceneLimits.
    static constexpr std::uint32_t MAX_VERTEX_BUFFERS = SceneLimits::kMaxVertexBuffers;
    static constexpr std::uint32_t MAX_INDEX_BUFFERS = SceneLimits::kMaxIndexBuffers;
    // Out-of-line UV buffer table (engine set 2, binding 1). Same block-growth
    // model as the vertex table; a mesh without out-of-line UVs stores a
    // sentinel in its vertex entry and never indexes this table.
    static constexpr std::uint32_t MAX_UV_BUFFERS = SceneLimits::kMaxUvBuffers;
    // Skin-influence table size (deform set bindings 0 and 1). Same block-growth
    // model as the vertex table: a skinned mesh allocates one block in this heap
    // and addresses it by slot.
    static constexpr std::uint32_t MAX_SKIN_BUFFERS = SceneLimits::kMaxSkinBuffers;
    // Entries per storage-buffer block. Single source: SceneLimits; mirrored in
    // shaders/scene_block_layout.slang.
    static constexpr std::uint32_t BLOCK_ENTRIES = SceneLimits::kEntriesPerBlock;
    // Draw-key table capacity: the full technique/draw-group bit width, owned
    // by TechniquePacking (TECHNIQUE_BITS = 14 -> 16384). Every per-key GPU
    // table (flags, counts, results, command regions) is sized to this, and the
    // collect push constants carry the live count, never a hardcoded 256. A key
    // can never exceed this: the low TECHNIQUE_BITS of a packed
    // technique_material are the key, and registration caps the count.
    static constexpr std::uint32_t MAX_DRAW_GROUPS =
        VulkanEngine::TechniqueManager::TechniquePacking::MAX_DRAW_GROUPS;

    // Highest Hi-Z level covered by whole HIZ_BATCH-sized DispatchHiZGen
    // iterations for a `mip_count`-level pyramid. The cull shaders clamp their
    // sampling independently (MaxHizMip() = floor(log2(max(hizW, hizH))));
    // Initialize asserts the two derivations agree so a sampled level can never
    // be left unwritten.
    static constexpr std::uint32_t LastHiZLevelWritten(std::uint32_t mip_count) {
        std::uint32_t last = 0;
        for (std::uint32_t bl = 0; bl < mip_count; bl += HIZ_BATCH) {
            last = bl + ((mip_count - bl < HIZ_BATCH) ? (mip_count - bl) : HIZ_BATCH) - 1;
        }
        return last;
    }

    SceneRenderer() = default;
    ~SceneRenderer();

    SceneRenderer(const SceneRenderer&) = delete;
    SceneRenderer& operator=(const SceneRenderer&) = delete;

    bool Initialize(VulkanBackend::Vulkan::IVulkanBootstrap& backend,
                    VulkanEngine::GpuResources::DeviceBufferHeap& vertex_heap,
                    SceneCapacity initial_capacity,
                    ShaderSystem::ShaderManager& shader_mgr,
                    ShaderSystem::PipelineFactory& pipeline_factory,
                    const EngineShaderIds& shader_ids,
                    std::uint32_t frames_in_flight,
                    DrawMode draw_mode,
                    Modifiers::ModifierManager& modifier_mgr);

    void Shutdown();

    // Grows (never shrinks) the indexed-drawing frame ring to cover the given
    // scene totals. Called from the gather pass once the frame's real totals
    // are known. Device-idles and re-creates the capacity-dependent buffers
    // when growth is required; no-op otherwise.
    void EnsureSceneCapacity(const SceneCapacity& required);

    // Switches draw mode at runtime: device idle, destroy + re-create the
    // mode-dependent buffers at the current capacity, then rebuild the
    // draw-mode-specialized compaction pipelines (expand, cull, collect, depth).
    // Descriptor set layouts and pools are mode-independent and are reused. The
    // main-pass technique pipelines are owned by TechniqueManager and are NOT
    // rebuilt here: the caller must re-specialize them too (GameEngine::SetDrawMode
    // does this).
    void Reinitialize(DrawMode mode);

    // Re-creates the resolution-dependent Hi-Z image ring and sampler when the
    // swapchain render extent changes (window resize). Idempotent: no-op when
    // the extent already matches the current allocation. Called once per frame
    // from the renderer after the swapchain extent is known. The render graph
    // needs no rebuild: the "hiz-image" resolver re-reads the handles each
    // frame.
    void EnsureRenderExtent(std::uint32_t width, std::uint32_t height);

    [[nodiscard]] DrawMode GetDrawMode() const { return draw_mode_; }
    [[nodiscard]] const SceneCapacity& GetSceneCapacity() const { return scene_capacity_; }
    // Runtime submesh budget: the block count chosen at Initialize, clamped by
    // the device's descriptor limits under SceneLimits::kMaxBlocksCeiling. The
    // gather pass clamps to MaxSubmeshes() so a scene can never write past the
    // descriptor binding arrays.
    [[nodiscard]] std::uint32_t MaxBlocks() const { return max_blocks_; }
    [[nodiscard]] std::uint32_t MaxSubmeshes() const {
        return max_blocks_ * SceneLimits::kEntriesPerBlock;
    }
    [[nodiscard]] bool IsDrawModeSupported(DrawMode mode) const;
    // Resolves a requested draw mode against the device capabilities: MID needs
    // both drawIndirectCount and drawIndirectFirstInstance, otherwise the result
    // is CID. This is the single selection rule used by Initialize and by
    // a runtime switch, so the two cannot disagree. Call after Initialize.
    [[nodiscard]] DrawMode ResolveDrawMode(DrawMode requested) const;
    [[nodiscard]] std::uint32_t GetTechniqueRegionTotal() const { return region_total_; }

    // MID main pass: CPU prefix-sum of submesh counts per technique. Defines
    // each technique's fixed command region in the shared command buffer.
    void SetTechniqueCommandRegions(std::span<const std::uint32_t> submeshes_per_technique);

    [[nodiscard]] vk::DescriptorSetLayout* GetSubmeshVertexEntriesLayout() const;
    [[nodiscard]] vk::DescriptorSetLayout* GetVertexBuffersLayout() const;
    [[nodiscard]] vk::DescriptorSetLayout* GetIndirectionLayout() const;

    void UpdateVertexBufferArrayElement(std::uint32_t frame_index, std::uint32_t buffer_index, vk::Buffer buffer, std::uint64_t size);
    void UpdateIndexBufferArrayElement(std::uint32_t frame_index, std::uint32_t buffer_index, vk::Buffer buffer, std::uint64_t size);
    // Out-of-line UV table (set 2, binding 1).
    void UpdateUvBufferArrayElement(std::uint32_t frame_index, std::uint32_t buffer_index, vk::Buffer buffer, std::uint64_t size);
    void UpdateAllFrameVertexBufferArrayElements(std::uint32_t buffer_index, vk::Buffer buffer, std::uint64_t size);
    void UpdateAllFrameUvBufferArrayElements(std::uint32_t buffer_index, vk::Buffer buffer, std::uint64_t size);
    void UpdateAllFrameIndexBufferArrayElements(std::uint32_t buffer_index, vk::Buffer buffer, std::uint64_t size);
    // Skin-influence tables (deform set, bindings 0 and 1). Written as mesh skin
    // streams become resident, exactly like the vertex/UV tables: the deform set
    // outlives the frame buffers, so a newly allocated skin block has to be
    // published to every frame in flight.
    void UpdateSkinBufferArrayElement(std::uint32_t frame_index, std::uint32_t buffer_index, vk::Buffer buffer, std::uint64_t size);
    void UpdateAllFrameSkinBufferArrayElements(std::uint32_t buffer_index, vk::Buffer buffer, std::uint64_t size);
    void UpdateSkinExtraBufferArrayElement(std::uint32_t frame_index, std::uint32_t buffer_index, vk::Buffer buffer, std::uint64_t size);
    void UpdateAllFrameSkinExtraBufferArrayElements(std::uint32_t buffer_index, vk::Buffer buffer, std::uint64_t size);

    // ── Per-frame deform state, produced by gather ──
    // The SkinEntry block array for `frame_index`. Gather grows it with
    // EnsureCapacity(total_submeshes) before writing: the array's Get() returns
    // nullptr past BlockCount(), and PrepareCompute runs after gather.
    [[nodiscard]] VulkanEngine::GpuResources::BlockArray& GetFrameSkinEntries(std::uint32_t frame_index);
    // The mapped flat bone-palette buffer for `frame_index` (one entry per
    // palette matrix). Empty when the buffer could not be mapped.
    [[nodiscard]] std::span<std::array<float, 16>> MapFrameBonePalettes(std::uint32_t frame_index);
    void UnmapFrameBonePalettes(std::uint32_t frame_index);
    // The mapped flat modifier-parameter buffer, or an empty span when it could
    // not be mapped (gather then drops app modifiers for the frame).
    [[nodiscard]] std::span<std::byte> MapFrameModifierParams(std::uint32_t frame_index);
    void UnmapFrameModifierParams(std::uint32_t frame_index);
    // Copies the frame's dispatch list. It is never uploaded; the deform stage
    // iterates it on the CPU.
    void SetModifierDispatches(std::uint32_t frame_index, std::span<const ModifierDispatch> dispatches);
    [[nodiscard]] std::span<const ModifierDispatch> GetModifierDispatches(std::uint32_t frame_index) const;
    // True once a modifier's compute pipeline exists and is usable. Gather only
    // emits a stage whose pipeline is ready, so an unbuilt modifier is skipped
    // rather than dispatched into a null pipeline.
    [[nodiscard]] bool IsModifierReady(Modifiers::ModifierId id) const;
    [[nodiscard]] Modifiers::ModifierId SkinningModifier() const;
    // Reconciles the registry into compute pipelines. Called once per frame in
    // the Bridge phase, before gather resolves the stack: a modifier registered
    // this frame must have a pipeline before gather can number its stage.
    void SyncModifierPipelines(std::uint32_t frame_counter);
    // Runs the frame's CPU dispatch list against the deform pipeline layout.
    // A no-op for an empty list (an unskinned scene dispatches nothing).
    void DispatchDeform(vk::CommandBuffer cmd, std::uint32_t frame_index);
    // The per-frame deformed-vertex buffer (set 2, binding 2 / deform set
    // binding 4). Exposed for diagnostics and the GPU tests that read a deform
    // result back; gather never touches it.
    [[nodiscard]] vk::Buffer GetFrameDeformVertices(std::uint32_t frame_index) const;
    // Per-submesh vertex and cull entries. Gather owns the other four block
    // arrays through GetFrameBlockArrays; these two are written by expand on
    // the GPU, so they are only seeded/read by diagnostics and the cull tests.
    [[nodiscard]] VulkanEngine::GpuResources::BlockArray& GetFrameVertexEntries(std::uint32_t frame_index);
    [[nodiscard]] VulkanEngine::GpuResources::BlockArray& GetFrameCullEntries(std::uint32_t frame_index);
    // Replaces the technique flag table contents (one packed word per technique
    // id). UpdateTechniqueFlags packs the registered techniques through this.
    void SetTechniqueFlags(std::span<const std::uint32_t> flags);
    // The registry this renderer resolves modifier ids against.
    [[nodiscard]] Modifiers::ModifierManager& GetModifierManager() { return *modifier_mgr_; }
    // True when the frame carries at least one deform dispatch. Expand reads it
    // to keep an unskinned scene off the skin-entry descriptors entirely.
    [[nodiscard]] bool HasActiveDeform() const { return has_active_deform_; }
    void SetHasActiveDeform(const bool active) { has_active_deform_ = active; }

    void PrepareCompute(vk::CommandBuffer cmd,
                        VulkanEngine::ComponentRegistry& registry,
                        const glm::mat4& view_matrix,
                        const glm::mat4& projection_matrix,
                        std::uint32_t width,
                        std::uint32_t height,
                        std::uint32_t frame_index);

    void PollShaders(std::uint32_t frame_counter);

    void DepthPrepass(vk::CommandBuffer cmd,
                      std::uint32_t width,
                      std::uint32_t height,
                      std::uint32_t frame_index);

    void Render(vk::CommandBuffer cmd,
                VulkanEngine::ComponentRegistry& registry,
                VulkanEngine::TechniqueManager::TechniqueManager& technique_mgr,
                VulkanEngine::BindlessManager::BindlessManager& bindless_mgr,
                const glm::mat4& projection_matrix,
                const glm::mat4& view_matrix,
                std::uint32_t width,
                std::uint32_t height,
                std::uint32_t frame_index);

    void DispatchHiZGen(vk::CommandBuffer cmd,
                        std::uint32_t width,
                        std::uint32_t height,
                        std::uint32_t frame_index,
                        std::uint32_t image_index);

    void DispatchOcclusion(vk::CommandBuffer cmd,
                           std::uint32_t frame_index);

    void DispatchCollect(vk::CommandBuffer cmd, std::uint32_t frame_index);

    void DispatchOccluderSelect(vk::CommandBuffer cmd,
                                 std::uint32_t width,
                                 std::uint32_t height,
                                 std::uint32_t frame_index);

    // Hardware raster of the selected occluder meshes into the depth prepass
    // buffer (load_op = CLEAR, store = STORE; render graph node).
    void OccluderPrepass(vk::CommandBuffer cmd,
                         std::uint32_t width,
                         std::uint32_t height,
                         std::uint32_t frame_index);

    void DispatchPreCull(vk::CommandBuffer cmd, std::uint32_t frame_index);

    // Packs each registered technique's PipelineFlags into the shared flag
    // table buffer (upload on change only). Called once per frame before
    // PrepareCompute.
    void UpdateTechniqueFlags(
        VulkanEngine::TechniqueManager::TechniqueManager& technique_mgr);

    void InitializeHizFirstFrame(vk::CommandBuffer cmd);

    void SetSubmeshes(const std::vector<VulkanEngine::SubMesh>& submeshes) { scene_submeshes_ = submeshes; }
    [[nodiscard]] const std::vector<VulkanEngine::SubMesh>& GetSubmeshes() const { return scene_submeshes_; }

    struct FrameBlockArrays {
        VulkanEngine::GpuResources::BlockArray* dynamic_entries;
        VulkanEngine::GpuResources::BlockArray* static_entries;
        VulkanEngine::GpuResources::BlockArray* bounding_spheres;
        VulkanEngine::GpuResources::BlockArray* obb_entries;
    };
    [[nodiscard]] FrameBlockArrays GetFrameBlockArrays(std::uint32_t frame_index);

    [[nodiscard]] std::uint32_t GetCurrentEntityCount() const { return current_entity_count_; }
    void SetCurrentEntityCount(std::uint32_t count) { current_entity_count_ = count; }

    [[nodiscard]] const glm::mat4& GetViewProj() const { return view_proj_; }
    void SetViewProj(const glm::mat4& vp) { view_proj_ = vp; }

    [[nodiscard]] vk::Image GetHizImage(std::uint32_t frame_index) const {
        const auto& frame = frames_[frame_index % frames_in_flight_];
        return static_cast<vk::Image>(*frame.hiz_image);
    }
    [[nodiscard]] vk::ImageView GetHizFullView(std::uint32_t frame_index) const {
        const auto& frame = frames_[frame_index % frames_in_flight_];
        return static_cast<vk::ImageView>(*frame.hiz_full_view);
    }
    void UpdateHizDepthBinding(std::uint32_t frame_index, vk::ImageView depth_view);

    void DispatchExpand(vk::CommandBuffer cmd, std::uint32_t object_count,
                        const glm::mat4& view_proj, std::uint32_t frame_index);

    // Engine-standard descriptor sets 1-3 for a frame (set 0 is the bindless
    // manager's, set 4 is GetSceneUniformSet()).
    [[nodiscard]] vk::DescriptorSet GetFrameSubmeshVertexEntriesSet(std::uint32_t frame_index) const {
        return frames_[frame_index % frames_in_flight_].submesh_vertex_set.GetHandle();
    }
    [[nodiscard]] vk::DescriptorSet GetFrameVertexBuffersSet(std::uint32_t frame_index) const {
        return static_cast<vk::DescriptorSet>(*frames_[frame_index % frames_in_flight_].vertex_buffers_set);
    }
    [[nodiscard]] vk::DescriptorSet GetFrameIndirectionSet(std::uint32_t frame_index) const {
        return *frames_[frame_index % frames_in_flight_].indirection_raw_set;
    }

    // ── Lighting system (descriptor set 4) — binding 0 = SceneHeader, binding 1 = Light[] BlockArray ──
    [[nodiscard]] vk::DescriptorSetLayout GetSceneUniformLayout() const {
        return *scene_uniform_layout_;
    }
    [[nodiscard]] vk::DescriptorSet GetSceneUniformSet() const {
        return *scene_uniform_set_;
    }
    void UploadLighting(const SceneHeader& header,
                        std::span<const Light> lights,
                        VulkanEngine::GpuResources::StagingPool& staging);

private:
    struct TechniqueResult { std::uint32_t offset; std::uint32_t count; };
    struct FrameResources {
        // Block-based per-submesh buffers
        VulkanEngine::GpuResources::BlockArray dynamic_entries{};
        VulkanEngine::GpuResources::BlockArray static_entries{};
        VulkanEngine::GpuResources::BlockArray bounding_spheres{};
        VulkanEngine::GpuResources::BlockArray obb_entries{};
        VulkanEngine::GpuResources::BlockArray submesh_vertex_entries{};
        VulkanEngine::GpuResources::BlockArray cull_entries{};
        // Per-submesh deform addressing (expand set binding 7, deform set binding
        // 3). Written by gather; expand fills its `deform_base`.
        VulkanEngine::GpuResources::BlockArray skin_entries{};

        // Descriptor-write cursors for the block arrays above. They grow in
        // lockstep and only ever grow, so the descriptors for blocks that
        // already exist do not need rewriting every frame. `epoch` matches
        // SceneRenderer::binding_epoch_; when it differs the whole array is
        // rewritten (capacity growth recreated the underlying buffers).
        //
        // One cursor per (descriptor set, binding) pair. Each block array is
        // bound into its own (set, binding), and every pair has to write its own
        // blocks: a single shared cursor lets the first pair write its blocks
        // and then mark the frame's whole block budget written, so every later
        // binding keeps a descriptor that was never written (or, after a
        // capacity change, a stale one). That is a GPU hang, not a wrong pixel.
        enum class BlockBindingSlot : std::uint32_t {
            expand_dynamic_entries = 0,
            expand_static_entries,
            expand_submesh_vertex,
            expand_cull,
            occluder_select_cull,
            occluder_select_submesh_vertex,
            occluder_select_obb,
            submesh_vertex,
            occlusion_submesh_vertex,
            occlusion_cull,
            occlusion_bounding_spheres,
            occlusion_obb,
            collect_cull,
            // The skin-entry table is bound into both sets that can read it, and
            // each binding owns its own cursor for the same reason as the rest.
            expand_skin_entries,
            deform_skin_entries,
            count,
        };
        struct BlockBindingCache {
            std::uint32_t written_blocks = 0;
            std::uint32_t epoch = 0;
        };
        std::array<BlockBindingCache,
                   static_cast<std::size_t>(BlockBindingSlot::count)>
            block_binding_cache{};

        // ── Indexed-drawing substrate ──
        // One VertexIndirectionEntry per slot in each submesh's tight vertex
        // window. CID-only content: MID binds a fixed 8 B dummy and
        // never writes or reads it (it carries the addressing in firstInstance
        // and the index buffer).
        VulkanEngine::GpuResources::GpuBuffer vertex_indirection{};
        // CID: 4 B absolute slot indices into vertex_indirection, one
        // per occurrence. MID: 4 B absolute vertex indices, one per occurrence
        // (bound as the index buffer).
        VulkanEngine::GpuResources::GpuBuffer draw_indices{};
        // 2 x u32 atomic counters allocated by expand: [0]=entryBase, [1]=indexBase.
        VulkanEngine::GpuResources::GpuBuffer expand_counter{};

        // ── CID destinations (4 B compact index lists) ──
        VulkanEngine::GpuResources::GpuBuffer main_indices{};
        VulkanEngine::GpuResources::GpuBuffer depth_indices{};
        VulkanEngine::GpuResources::GpuBuffer occluder_indices{};
        // CID GPU-accumulated indexed draw commands (word 0 = indexCount).
        VulkanEngine::GpuResources::GpuBuffer depth_out_draw_command{};
        VulkanEngine::GpuResources::GpuBuffer occluder_out_draw_command{};

        // ── MID destinations (20 B indexed indirect commands + count) ──
        VulkanEngine::GpuResources::GpuBuffer main_commands{};
        VulkanEngine::GpuResources::GpuBuffer depth_commands{};
        VulkanEngine::GpuResources::GpuBuffer occluder_commands{};
        VulkanEngine::GpuResources::GpuBuffer depth_out_command_count{};
        VulkanEngine::GpuResources::GpuBuffer occluder_out_command_count{};
        // CPU prefix sum of submeshes per technique (host-visible, MID main pass).
        VulkanEngine::GpuResources::GpuBuffer technique_region_bases{};

        VulkanEngine::GpuResources::GpuBuffer occluder_candidate_count{};
        VulkanEngine::GpuResources::GpuBuffer technique_draw_commands{};
        VulkanEngine::GpuResources::GpuBuffer technique_counts{};
        VulkanEngine::GpuResources::GpuBuffer technique_results{};

        // ── Deform inputs ──
        // Flat bone-palette matrices, addressed by one global index
        // (`SkinEntry::palette_base + joint`). Deliberately not a BlockArray: a
        // descriptor array is one descriptor per block, so a palette that starts
        // mid-block or spans two blocks could not be addressed as one buffer.
        VulkanEngine::GpuResources::GpuBuffer bone_palettes{};
        // Flat modifier-parameter blocks. Host-visible/coherent and mapped during
        // gather; sized by SceneCapacity::modifier_param_bytes.
        VulkanEngine::GpuResources::GpuBuffer modifier_params{};
        // Deformed-vertex output: 2 * deform_vertex_span vertices, the A and B
        // regions the modifier stack ping-pongs between. Device-local; written
        // only by the deform pass and read only by the depth/main vertex
        // shaders (set 2, binding 2).
        VulkanEngine::GpuResources::GpuBuffer deform_vertices{};

        // Descriptor sets
        VulkanEngine::GpuResources::GpuDescriptorSet expand_set{};
        VulkanEngine::GpuResources::GpuDescriptorSet occlusion_set{};
        VulkanEngine::GpuResources::GpuDescriptorSet collect_set{};
        VulkanEngine::GpuResources::GpuDescriptorSet collect_write_set{};
        VulkanEngine::GpuResources::GpuDescriptorSet occluder_select_set{};
        VulkanEngine::GpuResources::GpuDescriptorSet submesh_vertex_set{};
        // Single shared indirection set (set 3) bound to vertex_indirection and
        // reused by the depth, occluder and main passes.
        vk::raii::DescriptorSet indirection_raw_set = vk::raii::DescriptorSet(nullptr);
        vk::raii::DescriptorSet vertex_buffers_set = vk::raii::DescriptorSet(nullptr);
        vk::raii::DescriptorSet index_buffers_set = vk::raii::DescriptorSet(nullptr);
        // Deform input set (set 1 of the deform pipeline). Allocated once at
        // Initialize; its skin tables are rewritten as skin streams become
        // resident, so it must outlive the capacity-dependent frame buffers.
        vk::raii::DescriptorSet deform_set = vk::raii::DescriptorSet(nullptr);
        VulkanEngine::GpuResources::GpuDescriptorSet hiz_set{};

        vk::raii::Image hiz_image = vk::raii::Image(nullptr);
        vk::raii::DeviceMemory hiz_memory = vk::raii::DeviceMemory(nullptr);
        std::vector<vk::raii::ImageView> hiz_mip_views{};
        vk::raii::ImageView hiz_full_view = vk::raii::ImageView(nullptr);

        // Projection coefficients for the occlusion culler (see OccPC.projInfo).
        glm::vec4 proj_info{};
    };

    // ── Built-in pipeline creation (table-driven, single submission) ──
    // Creates the Hi-Z set layout + pool, every scene pipeline layout, and
    // compiles all seven compute pipelines plus the depth-prepass pipeline in
    // one batched submission. Fails initialization on the first pipeline
    // creation error instead of silently booting without a pipeline.
    bool CreateScenePipelines(VulkanBackend::Vulkan::IVulkanBootstrap& backend,
                              ShaderSystem::ShaderManager& shader_mgr,
                              ShaderSystem::PipelineFactory& pipeline_factory,
                              const EngineShaderIds& shader_ids);


    void UpdateBlockArrayDescriptor(vk::DescriptorSet desc_set, std::uint32_t binding,
                                      VulkanEngine::GpuResources::BlockArray& block_buf,
                                      vk::DescriptorType desc_type);

    VulkanBackend::Vulkan::IVulkanBootstrap* backend_ = nullptr;

    // Hot-reload rebuild support: kept refs + per-pipeline creation descs so the
    // frame loop can re-create pipelines when a shader's version changes.
    ShaderSystem::ShaderManager* shader_mgr_ = nullptr;
    ShaderSystem::PipelineFactory* pipeline_factory_ = nullptr;
    Modifiers::ModifierManager* modifier_mgr_ = nullptr;
    EngineShaderIds shader_ids_{};

    // Set 1: SubmeshVertexEntries blocks
    std::unique_ptr<vk::raii::DescriptorSetLayout> submesh_vertex_layout_{};
    std::shared_ptr<VulkanEngine::GpuResources::DescriptorPool> submesh_vertex_pool_;

    // Set 2: Vertex buffer table
    std::unique_ptr<vk::raii::DescriptorSetLayout> vertex_buffers_layout_{};
    std::unique_ptr<vk::raii::DescriptorPool> vertex_buffers_pool_;

    // Set 3: Vertex-indirection buffer (depth uses dedicated set, main uses indirection_raw_set)
    std::unique_ptr<vk::raii::DescriptorSetLayout> indirection_layout_{};
    std::shared_ptr<VulkanEngine::GpuResources::DescriptorPool> indirection_pool_;
    std::unique_ptr<vk::raii::DescriptorPool> indirection_raw_pool_;

    // Set 4: Expand compute (block arrays + single buffers)
    std::unique_ptr<vk::raii::DescriptorSetLayout> expand_layout_{};
    std::shared_ptr<VulkanEngine::GpuResources::DescriptorPool> expand_pool_;
    std::unique_ptr<vk::raii::PipelineLayout> expand_pipeline_layout_{};
    ShaderSystem::PipelineSlot expand_slot_;
    std::optional<ShaderSystem::ComputePipelineDesc> expand_desc_;


    // Set 5: Occlusion compute (blocks + Hi-Z)
    std::unique_ptr<vk::raii::DescriptorSetLayout> occlusion_layout_{};
    std::shared_ptr<VulkanEngine::GpuResources::DescriptorPool> occlusion_pool_;
    std::unique_ptr<vk::raii::PipelineLayout> occlusion_pipeline_layout_{};
    ShaderSystem::PipelineSlot occlusion_slot_;
    std::optional<ShaderSystem::ComputePipelineDesc> occlusion_desc_;

    // Set 8: Occluder-select compute (cull blocks + transforms + OBBs + flag
    // table + indirection copy + draw command + selection counter)
    std::unique_ptr<vk::raii::DescriptorSetLayout> occluder_select_layout_{};
    std::shared_ptr<VulkanEngine::GpuResources::DescriptorPool> occluder_select_pool_;
    std::unique_ptr<vk::raii::PipelineLayout> occluder_select_pipeline_layout_{};
    ShaderSystem::PipelineSlot occluder_select_slot_;
    std::optional<ShaderSystem::ComputePipelineDesc> occluder_select_desc_;

    // Pre-cull compute: reuses the occlusion set (extended with the survivor
    // compaction bindings 6-8) + its own pipeline.
    std::unique_ptr<vk::raii::PipelineLayout> pre_cull_pipeline_layout_{};
    ShaderSystem::PipelineSlot pre_cull_slot_;
    std::optional<ShaderSystem::ComputePipelineDesc> pre_cull_desc_;

    // Set 6: Collect count + compact (cull blocks + indirections + intermediate)
    std::unique_ptr<vk::raii::DescriptorSetLayout> collect_layout_{};
    std::shared_ptr<VulkanEngine::GpuResources::DescriptorPool> collect_pool_;
    std::unique_ptr<vk::raii::PipelineLayout> collect_pipeline_layout_{};
    ShaderSystem::PipelineSlot collect_count_slot_;
    std::optional<ShaderSystem::ComputePipelineDesc> collect_count_desc_;
    // Collect write shaders (use separate layout)
    std::unique_ptr<vk::raii::DescriptorSetLayout> collect_write_layout_{};
    std::shared_ptr<VulkanEngine::GpuResources::DescriptorPool> collect_write_pool_;
    std::unique_ptr<vk::raii::PipelineLayout> collect_write_pipeline_layout_{};
    ShaderSystem::PipelineSlot collect_write_slot_;
    std::optional<ShaderSystem::ComputePipelineDesc> collect_write_desc_;

    std::unique_ptr<vk::raii::Sampler> depth_sampler_{};
    std::unique_ptr<vk::raii::Sampler> hiz_sampler_{};

    std::uint32_t depth_width_ = 0;
    std::uint32_t depth_height_ = 0;
    std::uint32_t hiz_mip_count_ = 0;
    bool hiz_initialized_ = false;

    std::unique_ptr<vk::raii::DescriptorSetLayout> empty_layout_{};
    std::shared_ptr<VulkanEngine::GpuResources::DescriptorPool> empty_pool_{};
    std::vector<VulkanEngine::GpuResources::GpuDescriptorSet> empty_sets_{};

    std::unique_ptr<vk::raii::PipelineLayout> depth_pipeline_layout_{};
    ShaderSystem::PipelineSlot depth_slot_;
    std::optional<ShaderSystem::GraphicsPipelineDesc> depth_desc_;

    std::unique_ptr<vk::raii::DescriptorSetLayout> hiz_layout_{};
    std::unique_ptr<vk::raii::PipelineLayout> hiz_pipeline_layout_{};
    ShaderSystem::PipelineSlot hiz_slot_;
    std::optional<ShaderSystem::ComputePipelineDesc> hiz_desc_;
    std::shared_ptr<VulkanEngine::GpuResources::DescriptorPool> hiz_pool_;

    // Bindless index buffer table (used by expand at set 5)
    std::unique_ptr<vk::raii::DescriptorSetLayout> index_buffers_layout_{};
    std::unique_ptr<vk::raii::DescriptorPool> index_buffers_pool_;

    // Deform input set (set 1 of the deform pipeline): skin tables, bone palette,
    // skin entries, deformed vertices and modifier parameters. Created here rather
    // than with the frame ring because the skin tables are written by the mesh
    // upload path, which must not target buffers that capacity growth re-creates.
    std::unique_ptr<vk::raii::DescriptorSetLayout> deform_layout_{};
    std::unique_ptr<vk::raii::DescriptorPool> deform_pool_{};
    // Deform pipeline layout: [vertex-buffer table (set 0), deform inputs (set
    // 1)] plus the 32-byte ModifierPC push constant. Every modifier pipeline
    // shares it, so the two sets stay bound across vkCmdBindPipeline.
    std::unique_ptr<vk::raii::PipelineLayout> deform_pipeline_layout_{};

    // One compute pipeline per live registry entry, keyed by ModifierId
    // (generation, value). A pipeline that is retired stays alive for
    // frames_in_flight_ frames: PipelineSlot's destructor destroys the Vulkan
    // pipeline immediately, and a submitted command buffer may still use it.
    struct ModifierPipeline {
        Modifiers::ModifierId id{};
        ShaderSystem::PipelineSlot slot{};
        std::optional<ShaderSystem::ComputePipelineDesc> desc{};
        // Shader version the current/last build used, plus the failure that was
        // already reported for it. A broken modifier is retried every frame (so a
        // transient driver failure heals without a shader edit) but logs once,
        // instead of spamming the log at frame rate.
        std::uint64_t shader_version = 0;
        bool failure_logged = false;
        bool ready = false;
        bool dead = false;
        std::uint64_t dead_at = 0;
    };
    std::map<std::uint64_t, ModifierPipeline> modifier_pipelines_{};
    std::uint64_t modifier_registry_version_ = 0;

    // Per-frame modifier dispatch list (CPU-only, never uploaded).
    std::vector<std::vector<ModifierDispatch>> frame_modifier_dispatches_{};
    bool has_active_deform_ = false;

    // Lighting system (set 4)
    VulkanEngine::GpuResources::GpuBuffer scene_header{};
    VulkanEngine::GpuResources::BlockArray scene_lights{};
    std::unique_ptr<vk::raii::DescriptorSetLayout> scene_uniform_layout_{};
    std::unique_ptr<vk::raii::DescriptorPool> scene_uniform_pool_{};
    std::unique_ptr<vk::raii::DescriptorSet> scene_uniform_set_{};

    std::vector<VulkanEngine::SubMesh> scene_submeshes_{};

    // Shared (not per-frame) technique flag table: PipelineFlags change rarely,
    // so a single host-visible buffer is updated in place only when contents
    // change. Worst case on a change is one frame reading a torn word, which is
    // self-correcting next frame.
    VulkanEngine::GpuResources::GpuBuffer technique_flags{};
    std::vector<std::uint32_t> technique_flags_cache_{};

    // Create/destroy the mode-dependent per-frame buffers at the current
    // capacity. Called by Initialize and EnsureSceneCapacity/Reinitialize.
    bool CreateFrameBuffers();
    void DestroyFrameBuffers();
    // Create/destroy the resolution-dependent Hi-Z image ring + sampler at the
    // current depth_width_/depth_height_. Called by Initialize and
    // EnsureRenderExtent. CreateHiZResources recomputes hiz_mip_count_, rewrites
    // the hiz-set storage/sampler/depth bindings, and asserts the generator's
    // mip coverage matches what the cull shaders sample.
    bool CreateHiZResources();
    void DestroyHiZResources();
    // Rebuild the draw-mode-specialized pipelines SceneRenderer owns: expand,
    // occluder-select, pre-cull, collect count/compact, collect write, depth.
    bool RebuildCompactionPipelines();

    // Runtime-sized per-frame resource ring (see frames_in_flight_).
    std::vector<FrameResources> frames_;
    std::uint32_t frames_in_flight_ = 3;
    std::uint32_t current_entity_count_ = 0;
    glm::mat4 view_proj_{1.0f};

    DrawMode draw_mode_ = DrawMode::CID;
    bool draw_indirect_count_supported_ = false;
    bool draw_indirect_first_instance_supported_ = false;
    // MID requires both drawIndirectCount and drawIndirectFirstInstance; this
    // single resolved rule feeds selection, IsDrawModeSupported and Reinitialize.
    bool mid_supported_ = false;
    SceneCapacity scene_capacity_{};
    // Chosen in Initialize from SceneLimits::MaxBlocksForDevice(device limits),
    // clamped to [1, kMaxBlocksCeiling]. Every descriptor binding array, pool
    // size, and BlockArray limit derives from this.
    std::uint32_t max_blocks_ = 0;
    // Bumped whenever the per-frame block-array descriptors are invalidated
    // (CreateFrameBuffers). PrepareCompute compares it against each frame's
    // BlockBindingCache to choose a full vs incremental descriptor rewrite.
    std::uint32_t binding_epoch_ = 1;
    // CPU prefix sum (MID): region_base_[t] = first command slot of technique t.
    std::vector<std::uint32_t> region_base_{};
    std::vector<std::uint32_t> region_count_{};
    std::uint32_t region_total_ = 0;

    // Number of draw keys present in the tables above for the frame being
    // recorded. The GPU tables are sized to MAX_DRAW_GROUPS, but the collect
    // push constants and the flag-table upload carry this live count so a small
    // scene never scans the full capacity. Set by SetTechniqueCommandRegions,
    // advanced by UpdateTechniqueFlags, and read by DispatchCollect.
    std::uint32_t technique_count_ = 0;
};

}
