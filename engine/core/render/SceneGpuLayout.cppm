// The shape of every GPU resource the scene renderer owns, as data.
//
// One row per descriptor set, one row per binding, one row per per-frame buffer and one row per per-submesh block array.
// Layout creation, pool sizing, descriptor writes, buffer sizing and the per-stage descriptor-budget check all read the same rows, so a count can no longer be declared in one place and budgeted in another.
//
// The rows speak vk:: for the descriptor and buffer flag words, the same vocabulary DescriptorDecl already uses for application sets.
// vk:: enums and flag words are values, not a device, so the tables stay unit-testable without one.
//
// The capacity numbers themselves live in VulkanEngine.SceneLimits; this module imports them and never the reverse, so each number keeps exactly one home.

module;

export module VulkanEngine.SceneGpuLayout;

import std;
import std.compat;

import vulkan;

import VulkanEngine.SceneLimits;

export namespace VulkanEngine::SceneGpuLayout {

// The capacity constants the rows below are built from.
using namespace VulkanEngine::SceneLimits;

// Descriptor count sentinel, substituted with the runtime block budget (max_blocks_) when the layout and the pool are built.
inline constexpr std::uint32_t kBlocks = 0xFFFFFFFFu;
// Returned by the lookups below when the row does not exist.
inline constexpr std::uint32_t kNoBinding = 0xFFFFFFFFu;

// ── Set identities ──
//
// Order matters only in that kSceneSets is indexed by SetId; a static_assert below enforces it.
enum class SetId : std::uint8_t {
    submesh_vertex,
    vertex_buffers,
    indirection,
    index_buffers,
    deform,
    expand,
    occlusion,
    occluder_select,
    collect,
    collect_write,
    empty,
    scene_uniform,
    hiz,
    count,
};

inline constexpr std::size_t kSetCount = static_cast<std::size_t>(SetId::count);

// ── Descriptor sources ──
//
// Every binding names the engine object that fills it, so the descriptor-write pass is a loop over the same rows that built the layout.
// A binding whose source is `none` is written by the upload path (skin tables), or is an empty placeholder.

enum class SourceKind : std::uint8_t {
    none,
    block_array,   // one descriptor per block, written incrementally
    buffer,        // one descriptor for the whole buffer
    mode_buffer,   // CID and MID bind different members (collect binding 2)
    hiz_image,     // sampler + image view pair written by the Hi-Z ring
};

enum class BlockSlot : std::uint8_t {
    dynamic_entries,
    static_entries,
    bounding_spheres,
    obb_entries,
    submesh_vertex_entries,
    cull_entries,
    skin_entries,
    count,
};

inline constexpr std::size_t kBlockSlotCount = static_cast<std::size_t>(BlockSlot::count);

enum class BufferSlot : std::uint8_t {
    // Per-frame ring.
    vertex_indirection,
    draw_indices,
    expand_counter,
    main_indices,
    depth_indices,
    occluder_indices,
    main_commands,
    depth_commands,
    occluder_commands,
    technique_region_bases,
    depth_out_counter,
    depth_technique_counts,
    occluder_out_counter,
    occluder_technique_counts,
    technique_draw_commands,
    occluder_candidate_count,
    technique_counts,
    technique_results,
    bone_palettes,
    modifier_params,
    deform_vertices,
    // Singleton buffers owned by the renderer rather than by a frame.
    technique_flags,
    scene_header,
    count,
};

inline constexpr std::size_t kBufferSlotCount = static_cast<std::size_t>(BufferSlot::count);

// Who writes a binding's descriptors. PrepareCompute only re-points `prepare_compute` rows.
// The rest belong to Initialize, the mesh upload path, the Hi-Z ring or the lighting upload.
// Writing those from the frame loop would either duplicate work or point a descriptor at a buffer that does not exist yet.
// The default is `external` so a row that says nothing is left alone, which is the harmless direction.
enum class Writer : std::uint8_t { prepare_compute, external };

// Draw-mode applicability of a binding. `collect_write` is only filled in CID, where its command buffer exists at all.
enum class BindingMode : std::uint8_t { both, cid_only };

struct BindingSpec {
    std::uint32_t binding = 0;
    vk::DescriptorType kind = vk::DescriptorType::eStorageBuffer;
    std::uint32_t count = 1;             // kBlocks, or a literal
    vk::ShaderStageFlags stages = vk::ShaderStageFlagBits::eCompute;
    vk::DescriptorBindingFlags flags{};
    SourceKind source = SourceKind::none;
    Writer writer = Writer::external;
    BindingMode mode = BindingMode::both;
    BlockSlot block_slot = BlockSlot::dynamic_entries;   // source == block_array
    BufferSlot buffer_slot = BufferSlot::draw_indices;   // source == buffer/mode_buffer
    BufferSlot buffer_slot_mid = BufferSlot::draw_indices; // source == mode_buffer
};

// Row constructors, so the tables read as one line per binding.
// The field order stays in the struct so a writer can still spell a row out longhand.
[[nodiscard]] constexpr BindingSpec Block(
    const std::uint32_t binding, const vk::ShaderStageFlags stages,
    const vk::DescriptorBindingFlags flags, const BlockSlot slot, const Writer writer,
    const BindingMode mode = BindingMode::both) noexcept {
    BindingSpec s{};
    s.binding = binding;
    s.kind = vk::DescriptorType::eStorageBuffer;
    s.count = kBlocks;
    s.stages = stages;
    s.flags = flags;
    s.source = SourceKind::block_array;
    s.writer = writer;
    s.mode = mode;
    s.block_slot = slot;
    return s;
}

[[nodiscard]] constexpr BindingSpec Buf(
    const std::uint32_t binding, const vk::ShaderStageFlags stages,
    const BufferSlot slot, const Writer writer,
    const vk::DescriptorBindingFlags flags = {},
    const BindingMode mode = BindingMode::both) noexcept {
    BindingSpec s{};
    s.binding = binding;
    s.stages = stages;
    s.flags = flags;
    s.source = SourceKind::buffer;
    s.writer = writer;
    s.mode = mode;
    s.buffer_slot = slot;
    return s;
}

[[nodiscard]] constexpr BindingSpec ModeBuf(
    const std::uint32_t binding, const vk::ShaderStageFlags stages,
    const BufferSlot cid_slot, const BufferSlot mid_slot, const Writer writer) noexcept {
    BindingSpec s{};
    s.binding = binding;
    s.stages = stages;
    s.source = SourceKind::mode_buffer;
    s.writer = writer;
    s.buffer_slot = cid_slot;
    s.buffer_slot_mid = mid_slot;
    return s;
}

// A binding with a descriptor array (a fixed table or a Hi-Z array); `kind` distinguishes sampled image, sampler, storage image and storage buffer.
[[nodiscard]] constexpr BindingSpec Array(
    const std::uint32_t binding, const vk::DescriptorType kind, const std::uint32_t count,
    const vk::ShaderStageFlags stages, const vk::DescriptorBindingFlags flags,
    const SourceKind source, const Writer writer) noexcept {
    BindingSpec s{};
    s.binding = binding;
    s.kind = kind;
    s.count = count;
    s.stages = stages;
    s.flags = flags;
    s.source = source;
    s.writer = writer;
    return s;
}

struct SetSpec {
    SetId id = SetId::count;
    std::string_view name{};                 // "<name>-layout", "<name>-pool"
    std::span<const BindingSpec> bindings{};
    // Non-zero replaces frames_in_flight_ as the pool's maxSets, and the row then owns the one set allocated from that pool instead of the per-frame ring.
    std::uint32_t fixed_sets = 0;
};

// ── The tables ──
//
// One row per descriptor set, one row per binding inside it.
// Binding numbers, descriptor counts, stage masks, binding flags and the pool budget all come from these rows, so a count can no longer be declared in one place and budgeted in another.

namespace Bindings {

using enum vk::DescriptorType;

inline constexpr vk::ShaderStageFlags V = vk::ShaderStageFlagBits::eVertex;
inline constexpr vk::ShaderStageFlags C = vk::ShaderStageFlagBits::eCompute;
inline constexpr vk::ShaderStageFlags VF =
    vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;
inline constexpr vk::ShaderStageFlags VC =
    vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eCompute;

inline constexpr vk::DescriptorBindingFlags PUB =
    vk::DescriptorBindingFlagBits::ePartiallyBound |
    vk::DescriptorBindingFlagBits::eUpdateAfterBind;
inline constexpr vk::DescriptorBindingFlags UAB =
    vk::DescriptorBindingFlagBits::eUpdateAfterBind;
inline constexpr vk::DescriptorBindingFlags PUBV =
    vk::DescriptorBindingFlagBits::ePartiallyBound |
    vk::DescriptorBindingFlagBits::eUpdateAfterBind |
    vk::DescriptorBindingFlagBits::eVariableDescriptorCount;

// Set 1: submesh vertex entries. A plain block array with no update-after-bind flag, because gather only ever appends blocks and the descriptor is written before the set is bound.
inline constexpr std::array kSubmeshVertex{
    Block(0, VC, {}, BlockSlot::submesh_vertex_entries, Writer::prepare_compute),
};

// Set 2: vertex buffer table (binding 0), out-of-line UV table (binding 1) and the per-frame deformed-vertex buffer (binding 2).
// The two tables are fixed count: the spec allows the variable-count flag only on the highest binding, and neither array's runtime length is ever queried.
inline constexpr std::array kVertexBuffers{
    Array(0, eStorageBuffer, kMaxVertexBuffers, VC, PUB, SourceKind::none, Writer::external),
    Array(1, eStorageBuffer, kMaxUvBuffers, VC, PUB, SourceKind::none, Writer::external),
    Buf(2, V, BufferSlot::deform_vertices, Writer::prepare_compute),
};

// Set 3: vertex indirection. Update-after-bind so the frame loop can re-point it without a layout change.
inline constexpr std::array kIndirection{
    Buf(0, V, BufferSlot::vertex_indirection, Writer::prepare_compute, UAB),
};

// Index buffer table (bindless, update-after-bind) for expand, one set per frame.
inline constexpr std::array kIndexBuffers{
    Array(0, eStorageBuffer, kMaxIndexBuffers, C, PUBV, SourceKind::none, Writer::external),
};

// Deform input set (set 1 of the deform pipeline): skin-influence table, extra influence table, flat bone palette, per-submesh SkinEntry block array, deformed-vertex output and the flat modifier parameters.
//
// The two skin tables are fixed descriptor arrays with the partially-bound and update-after-bind flags, because a mesh upload publishes a newly allocated skin block from the middle of the frame loop while earlier frames may still be reading the set.
inline constexpr std::array kDeform{
    Array(0, eStorageBuffer, kMaxSkinBuffers, C, PUB, SourceKind::none, Writer::external),
    Array(1, eStorageBuffer, kMaxSkinBuffers, C, PUB, SourceKind::none, Writer::external),
    Buf(2, C, BufferSlot::bone_palettes, Writer::prepare_compute),
    Block(3, C, {}, BlockSlot::skin_entries, Writer::prepare_compute),
    Buf(4, C, BufferSlot::deform_vertices, Writer::prepare_compute),
    Buf(5, C, BufferSlot::modifier_params, Writer::prepare_compute),
};

// Set 4: expand. Five block arrays plus vertex_indirection, draw_indices and the expand counter.
// The fifth block array (the per-submesh SkinEntry table) sits at binding 7 so the three single bindings keep the numbers the shader already uses.
inline constexpr std::array kExpand{
    Block(0, C, {}, BlockSlot::dynamic_entries, Writer::prepare_compute),
    Block(1, C, {}, BlockSlot::static_entries, Writer::prepare_compute),
    Block(2, C, {}, BlockSlot::submesh_vertex_entries, Writer::prepare_compute),
    Block(3, C, {}, BlockSlot::cull_entries, Writer::prepare_compute),
    Buf(4, C, BufferSlot::vertex_indirection, Writer::prepare_compute),
    Buf(5, C, BufferSlot::draw_indices, Writer::prepare_compute),
    Buf(6, C, BufferSlot::expand_counter, Writer::prepare_compute),
    Block(7, C, {}, BlockSlot::skin_entries, Writer::prepare_compute),
};

// Set 5: occlusion. Four block arrays, the Hi-Z combined image sampler, and the pre-cull survivor compaction bindings 6-11 that the pre-cull pass adds.
inline constexpr std::array kOcclusion{
    Block(0, C, {}, BlockSlot::submesh_vertex_entries, Writer::prepare_compute),
    Block(1, C, {}, BlockSlot::cull_entries, Writer::prepare_compute),
    Block(2, C, {}, BlockSlot::bounding_spheres, Writer::prepare_compute),
    Array(3, eCombinedImageSampler, 1, C, {}, SourceKind::hiz_image, Writer::prepare_compute),
    Block(4, C, {}, BlockSlot::obb_entries, Writer::prepare_compute),
    Buf(5, C, BufferSlot::technique_flags, Writer::prepare_compute),
    Buf(6, C, BufferSlot::draw_indices, Writer::prepare_compute),
    Buf(7, C, BufferSlot::depth_commands, Writer::prepare_compute),
    Buf(8, C, BufferSlot::depth_out_counter, Writer::prepare_compute),
    Buf(9, C, BufferSlot::technique_region_bases, Writer::prepare_compute),
    Buf(10, C, BufferSlot::depth_technique_counts, Writer::prepare_compute),
    Buf(11, C, BufferSlot::depth_indices, Writer::prepare_compute),
};

// Set 8: occluder select. Cull/vertex/OBB blocks plus the flag table, the full indirection, the occluder commands, three counters, the region bases, the per-group counts and the occluder index list.
inline constexpr std::array kOccluderSelect{
    Block(0, C, {}, BlockSlot::cull_entries, Writer::prepare_compute),
    Block(1, C, {}, BlockSlot::submesh_vertex_entries, Writer::prepare_compute),
    Block(2, C, {}, BlockSlot::obb_entries, Writer::prepare_compute),
    Buf(3, C, BufferSlot::technique_flags, Writer::prepare_compute),
    Buf(4, C, BufferSlot::draw_indices, Writer::prepare_compute),
    Buf(5, C, BufferSlot::occluder_commands, Writer::prepare_compute),
    Buf(6, C, BufferSlot::occluder_out_counter, Writer::prepare_compute),
    Buf(7, C, BufferSlot::occluder_candidate_count, Writer::prepare_compute),
    Buf(8, C, BufferSlot::technique_region_bases, Writer::prepare_compute),
    Buf(9, C, BufferSlot::occluder_technique_counts, Writer::prepare_compute),
    Buf(10, C, BufferSlot::occluder_indices, Writer::prepare_compute),
};

// Set 6: collect count/compact. One cull block array, the draw indices, the mode-dependent output (MID main commands, CID compact indices), the technique results, the region bases and the per-group counts.
inline constexpr std::array kCollect{
    Block(0, C, {}, BlockSlot::cull_entries, Writer::prepare_compute),
    Buf(1, C, BufferSlot::draw_indices, Writer::prepare_compute),
    ModeBuf(2, C, BufferSlot::main_indices, BufferSlot::main_commands,
            Writer::prepare_compute),
    Buf(3, C, BufferSlot::technique_results, Writer::prepare_compute),
    Buf(4, C, BufferSlot::technique_region_bases, Writer::prepare_compute),
    Buf(5, C, BufferSlot::technique_counts, Writer::prepare_compute),
};

// Set 7: collect write, CID only. Emits one indexed indirect command per technique from the technique results.
inline constexpr std::array kCollectWrite{
    Buf(0, C, BufferSlot::technique_results, Writer::prepare_compute, {},
        BindingMode::cid_only),
    Buf(1, C, BufferSlot::technique_draw_commands, Writer::prepare_compute, {},
        BindingMode::cid_only),
};

// Set 4 (engine side): scene header and the Light block table.
inline constexpr std::array kLighting{
    Buf(0, VF, BufferSlot::scene_header, Writer::external),
    Array(1, eStorageBuffer, kMaxLightBlocks, VF,
          vk::DescriptorBindingFlagBits::ePartiallyBound, SourceKind::none,
          Writer::external),
};

// Hi-Z generator: depth input, its sampler, and the storage-image array of mip levels.
// All three are rewritten by the Hi-Z ring on every re-create.
inline constexpr std::array kHiZ{
    Array(0, eSampledImage, 1, C, {}, SourceKind::hiz_image, Writer::external),
    Array(1, eSampler, 1, C, {}, SourceKind::hiz_image, Writer::external),
    Array(2, eStorageImage, kMaxHiZMips, C, {}, SourceKind::hiz_image, Writer::external),
};

} // namespace Bindings

inline constexpr std::array kSceneSets{
    SetSpec{SetId::submesh_vertex, "submesh-vertex", Bindings::kSubmeshVertex},
    SetSpec{SetId::vertex_buffers, "vertex-buffers", Bindings::kVertexBuffers},
    SetSpec{SetId::indirection, "indirection", Bindings::kIndirection},
    SetSpec{SetId::index_buffers, "index-buffers", Bindings::kIndexBuffers},
    SetSpec{SetId::deform, "deform", Bindings::kDeform},
    SetSpec{SetId::expand, "expand", Bindings::kExpand},
    SetSpec{SetId::occlusion, "occlusion", Bindings::kOcclusion},
    SetSpec{SetId::occluder_select, "occluder-select", Bindings::kOccluderSelect},
    SetSpec{SetId::collect, "collect", Bindings::kCollect},
    SetSpec{SetId::collect_write, "collect-write", Bindings::kCollectWrite},
    // The empty set is a placeholder for set 0 of the depth pipeline.
    // It has no bindings, so its pool carries no descriptor types either.
    SetSpec{SetId::empty, "empty", {}},
    SetSpec{SetId::scene_uniform, "scene-uniform", Bindings::kLighting, 1},
    SetSpec{SetId::hiz, "hiz", Bindings::kHiZ},
};

// A fixed row owns one set, allocated once from the pool its `fixed_sets` sizes: a second set would need a caller able to choose between them.
[[nodiscard]] constexpr bool FixedSetsAreSingle() noexcept {
    for (const SetSpec& set : kSceneSets) {
        if (set.fixed_sets > 1) return false;
    }
    return true;
}
static_assert(FixedSetsAreSingle(), "fixed_sets is 0 or 1: a fixed row allocates exactly one set");

static_assert(kSceneSets.size() == kSetCount,
              "kSceneSets must hold exactly one row per SetId, in SetId order");
static_assert(SetId::submesh_vertex == SetId(0) && SetId::hiz == SetId(kSetCount - 1),
              "kSceneSets is indexed by SetId, so the enum order is the table order");

// ── Derivations ──
//

[[nodiscard]] constexpr const SetSpec& SetOf(const SetId id) noexcept {
    return kSceneSets[static_cast<std::size_t>(id)];
}

// Number of descriptors of one kind a set needs at `blocks` blocks.
[[nodiscard]] constexpr std::uint32_t CountOf(const SetId id, const vk::DescriptorType kind,
                                              const std::uint32_t blocks) noexcept {
    std::uint32_t n = 0;
    for (const BindingSpec& b : SetOf(id).bindings) {
        if (b.kind != kind) continue;
        n += (b.count == kBlocks ? blocks : b.count);
    }
    return n;
}

// Storage-buffer demand, split into the part that scales with the block budget and the fixed part.
// MaxBlocksForDevice solves limits against this pair.
struct StorageFit {
    std::uint32_t blocks = 0;
    std::uint32_t fixed = 0;
};

[[nodiscard]] constexpr StorageFit StorageFitOf(const SetId id) noexcept {
    StorageFit fit{};
    for (const BindingSpec& b : SetOf(id).bindings) {
        if (b.kind != vk::DescriptorType::eStorageBuffer) continue;
        if (b.count == kBlocks) {
            ++fit.blocks;
        } else {
            fit.fixed += b.count;
        }
    }
    return fit;
}

// Total storage buffers in one set, matching the layout at `blocks` blocks.
[[nodiscard]] constexpr std::uint32_t StorageBuffersForSet(const SetId id,
                                                           const std::uint32_t blocks) noexcept {
    const StorageFit fit = StorageFitOf(id);
    return fit.blocks * blocks + fit.fixed;
}

// The depth and main graphics pipelines bind the empty, submesh-vertex, vertex and indirection sets on one stage, so the per-stage limit applies to their sum.
// Derived from the rows rather than restated, so it cannot drift.
[[nodiscard]] constexpr StorageFit DepthGraphicsStageFit() noexcept {
    const StorageFit vertex = StorageFitOf(SetId::submesh_vertex);
    const StorageFit buffers = StorageFitOf(SetId::vertex_buffers);
    const StorageFit indirection = StorageFitOf(SetId::indirection);
    return StorageFit{vertex.blocks + buffers.blocks + indirection.blocks,
                      vertex.fixed + buffers.fixed + indirection.fixed};
}

// A layout that carries any update-after-bind binding MUST be created with eUpdateAfterBindPool.
// Deriving that from the rows keeps the pool flag, the layout flag and the binding flags from disagreeing.
[[nodiscard]] constexpr bool UpdatesAfterBind(const SetId id) noexcept {
    for (const BindingSpec& b : SetOf(id).bindings) {
        if (static_cast<bool>(b.flags & vk::DescriptorBindingFlagBits::eUpdateAfterBind)) {
            return true;
        }
    }
    return false;
}

// The binding that uses eVariableDescriptorCount, or kNoBinding.
// The spec allows at most one, and only on the highest binding of the set.
[[nodiscard]] constexpr std::uint32_t VariableBinding(const SetId id) noexcept {
    std::uint32_t found = kNoBinding;
    for (const BindingSpec& b : SetOf(id).bindings) {
        if (static_cast<bool>(b.flags & vk::DescriptorBindingFlagBits::eVariableDescriptorCount)) {
            found = b.binding;
        }
    }
    return found;
}

// The descriptor count the layout declares for a binding: the block budget for a kBlocks row, the literal otherwise.
[[nodiscard]] constexpr std::uint32_t DescriptorCount(const BindingSpec& binding,
                                                      const std::uint32_t blocks) noexcept {
    return binding.count == kBlocks ? blocks : binding.count;
}

// Highest binding number + 1, for a debug assertion against the layout limit.
[[nodiscard]] constexpr std::uint32_t BindingCount(const SetId id) noexcept {
    std::uint32_t n = 0;
    for (const BindingSpec& b : SetOf(id).bindings) {
        n = std::max(n, b.binding + 1);
    }
    return n;
}

// The nth binding of a kind, or kNoBinding.
// Used where the shader needs one specific binding and the table is the only place its number lives.
[[nodiscard]] constexpr std::uint32_t BindingOfKind(const SetId id,
                                                    const vk::DescriptorType kind,
                                                    const std::uint32_t nth = 0) noexcept {
    std::uint32_t seen = 0;
    for (const BindingSpec& b : SetOf(id).bindings) {
        if (b.kind != kind) continue;
        if (seen == nth) return b.binding;
        ++seen;
    }
    return kNoBinding;
}

// ── Descriptor pool budget ──
//

struct PoolCounts {
    std::uint32_t max_sets = 0;
    std::uint32_t storage_buffers = 0;
    std::uint32_t combined_image_samplers = 0;
    std::uint32_t sampled_images = 0;
    std::uint32_t storage_images = 0;
    std::uint32_t samplers = 0;
};

// Pool sizes for one set, derived from the binding rows, so a binding added to a layout can never be missing from the pool (or vice versa).
[[nodiscard]] constexpr PoolCounts PoolCountsFor(const SetId id, const std::uint32_t blocks,
                                                 const std::uint32_t frames) noexcept {
    const SetSpec& set = SetOf(id);
    const std::uint32_t sets = set.fixed_sets != 0 ? set.fixed_sets : frames;
    PoolCounts counts{};
    counts.max_sets = sets;
    counts.storage_buffers = CountOf(id, vk::DescriptorType::eStorageBuffer, blocks) * sets;
    counts.combined_image_samplers =
        CountOf(id, vk::DescriptorType::eCombinedImageSampler, blocks) * sets;
    counts.sampled_images = CountOf(id, vk::DescriptorType::eSampledImage, blocks) * sets;
    counts.storage_images = CountOf(id, vk::DescriptorType::eStorageImage, blocks) * sets;
    counts.samplers = CountOf(id, vk::DescriptorType::eSampler, blocks) * sets;
    return counts;
}

// ── Block-binding cursors ──
//
// Every block-array binding owns its own descriptor-write cursor.
// The same BlockArray is bound into several sets, and a single shared cursor would mark the frame's whole block budget written after the first pair, leaving every later binding with a descriptor that was never written (a GPU hang, not a wrong pixel).
// The flat index into the cursor array is the row's position, so the cursor set cannot drift from the tables.

[[nodiscard]] constexpr std::uint32_t BlockBindingCount() noexcept {
    std::uint32_t n = 0;
    for (const SetSpec& set : kSceneSets) {
        for (const BindingSpec& b : set.bindings) {
            if (b.source == SourceKind::block_array) ++n;
        }
    }
    return n;
}

[[nodiscard]] constexpr std::uint32_t BlockBindingIndex(const SetId id,
                                                        const std::uint32_t binding) noexcept {
    std::uint32_t index = 0;
    for (const SetSpec& set : kSceneSets) {
        for (const BindingSpec& b : set.bindings) {
            if (b.source != SourceKind::block_array) continue;
            if (set.id == id && b.binding == binding) return index;
            ++index;
        }
    }
    return kNoBinding;
}

// Descriptor limits the block budget depends on, kept free of Vulkan objects so the derivation is unit-testable without a device.
struct DeviceLimits {
    std::uint32_t max_descriptor_set_storage_buffers = 0;
    std::uint32_t max_per_stage_descriptor_storage_buffers = 0;
};

// Largest block count whose worst-case set fits every limit.
// Never returns 0: a device that cannot fit even one block still gets one, and the caller reports the condition rather than dividing by zero.
[[nodiscard]] constexpr std::uint32_t
MaxBlocksForDevice(const DeviceLimits& limits) noexcept {
    std::uint32_t best = kMaxBlocksCeiling;
    const auto fit_into = [&best](const StorageFit& fit, const std::uint32_t limit) {
        if (fit.blocks == 0) return;   // constant set: cannot be clamped by blocks
        if (limit <= fit.fixed) {
            // Even zero blocks would not fit this binding's fixed part.
            best = 0;
            return;
        }
        best = std::min(best, (limit - fit.fixed) / fit.blocks);
    };
    for (const SetSpec& set : kSceneSets) {
        const StorageFit fit = StorageFitOf(set.id);
        for (const std::uint32_t limit :
             {limits.max_descriptor_set_storage_buffers,
              limits.max_per_stage_descriptor_storage_buffers}) {
            fit_into(fit, limit);
        }
    }
    // The depth/main stage binds three sets at once; its sum is the per-stage constraint those sets individually cannot express.
    const StorageFit stage = DepthGraphicsStageFit();
    fit_into(stage, limits.max_descriptor_set_storage_buffers);
    fit_into(stage, limits.max_per_stage_descriptor_storage_buffers);
    return std::max<std::uint32_t>(best, 1u);
}

// Largest single-set storage-buffer count across all sets at `blocks` blocks.
[[nodiscard]] constexpr std::uint32_t
MaxStorageBuffersPerSet(const std::uint32_t blocks) noexcept {
    std::uint32_t max_buffers = 0;
    for (const SetSpec& set : kSceneSets) {
        max_buffers = std::max(max_buffers, StorageBuffersForSet(set.id, blocks));
    }
    return max_buffers;
}

// ── Per-frame GPU buffers ──
//
// One row per capacity-sized buffer, with the CID and MID variants side by side.
// A mode that has no buffer for a slot sets `present = false`.
// A mode that needs a placeholder sets `fixed_bytes` to the dummy size: the shader path is specialized out, but the descriptor is still bound.

// Which memory a buffer lives in. Two values, because the block-array loop also derives its BlockArray::MemoryMode from this field: memory and mode have to agree, so one field produces both.
enum class BufMemory : std::uint8_t { host_visible, device_local };

struct BufferFlags {
    vk::BufferUsageFlags usage{};
    vk::MemoryPropertyFlags memory{};
};

// How a buffer's size is derived. Every case is a function of SceneCapacity and of constants pinned by a static_assert in the renderer.
enum class SizeKind : std::uint8_t {
    vertex_indirection,   // vertex_span * 8
    draw_indices,         // index_count * 4
    compact_indices,      // index_count * 4
    command_list,         // submesh_count * kIndirectCommandBytes
    technique_counts,     // MAX_DRAW_GROUPS * 4
    technique_commands,   // MAX_DRAW_GROUPS * kIndirectCommandBytes
    technique_results,    // MAX_DRAW_GROUPS * sizeof(TechniqueResult)
    counter,              // sizeof(uint32)
    expand_counter,       // 3 * sizeof(uint32)
    bone_palettes,        // palette_matrix_count * 16 * sizeof(float)
    modifier_params,      // modifier_param_bytes
    deform_vertices,      // 2 * deform_vertex_span * sizeof(Vertex)
    scene_header,         // sizeof(SceneHeader)
};

struct SizeRule {
    SizeKind kind = SizeKind::counter;
    // Floor for the computed size.
    // Keeps the buffer at least one element wide, so a zero-capacity scene never allocates a zero-byte buffer, and keeps an indirect-command region at least one command, so the shader's slot math stays in range.
    std::uint64_t min_bytes = 1;
};

struct ModeSpec {
    bool present = true;
    // Non-zero replaces the SizeRule: the mode needs this exact size (a dummy).
    std::uint64_t fixed_bytes = 0;
    BufferFlags flags{};
};

struct BufferSpec {
    std::string_view name{};   // debug object name; "-dummy" is appended for a placeholder
    BufferSlot slot = BufferSlot::draw_indices;
    SizeRule size{};
    ModeSpec cid{};
    ModeSpec mid{};
};

namespace Flags {
// Usage words. Every engine buffer is a storage buffer, and every one of them is written through a transfer, so the two bits travel together.
inline constexpr vk::BufferUsageFlags StorageTransfer =
    vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst;
inline constexpr vk::BufferUsageFlags StorageTransferReadable =
    vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst |
    vk::BufferUsageFlagBits::eTransferSrc;
inline constexpr vk::BufferUsageFlags IndexTransfer =
    vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eIndexBuffer |
    vk::BufferUsageFlagBits::eTransferDst;
inline constexpr vk::BufferUsageFlags IndirectTransfer =
    vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eIndirectBuffer |
    vk::BufferUsageFlagBits::eTransferDst;
// Memory words. A host-visible buffer is coherent too: the renderer maps it directly and never flushes per range.
inline constexpr vk::MemoryPropertyFlags DeviceLocal =
    vk::MemoryPropertyFlagBits::eDeviceLocal;
inline constexpr vk::MemoryPropertyFlags HostVisible =
    vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
} // namespace Flags

// A mode's dummy binding: present, fixed size, storage-only usage, device-local.
inline constexpr ModeSpec kDummy4{true, 4, {Flags::StorageTransfer, Flags::DeviceLocal}};
inline constexpr ModeSpec kDummy8{true, 8, {Flags::StorageTransfer, Flags::DeviceLocal}};

// A command list must keep room for at least one command, so its floor is a record size rather than one byte.
// Every other row's formula already yields a usable size, so the default floor of 1 is all it needs.
inline constexpr std::uint64_t kTechniqueResultFloor = 8;

inline constexpr std::array kFrameBuffers{
    // vertex_indirection: CID carries one 8 B entry per vertex-window slot; MID binds a fixed 8 B dummy because it addresses vertices directly.
    BufferSpec{"vertex-indirection", BufferSlot::vertex_indirection,
               {SizeKind::vertex_indirection},
               {true, 0, {Flags::StorageTransfer, Flags::DeviceLocal}}, kDummy8},
    // draw_indices is bound as an index buffer only in MID, where it holds the absolute vertex index of each occurrence.
    // CID binds the compacted main/depth/occluder lists instead, and only ever writes this one through a storage descriptor, so the index usage bit belongs to the MID variant.
    BufferSpec{"draw-indices", BufferSlot::draw_indices, {SizeKind::draw_indices},
               {true, 0, {Flags::StorageTransfer, Flags::DeviceLocal}},
               {true, 0, {Flags::IndexTransfer, Flags::DeviceLocal}}},
    BufferSpec{"expand-counter", BufferSlot::expand_counter, {SizeKind::expand_counter},
               {true, 0, {Flags::StorageTransfer, Flags::HostVisible}},
               {true, 0, {Flags::StorageTransfer, Flags::HostVisible}}},
    // CID compact index lists. MID specializes the writing shader path out, so the descriptors are 4 B dummies (depth/occluder) or absent (main).
    BufferSpec{"main-indices", BufferSlot::main_indices, {SizeKind::compact_indices},
               {true, 0, {Flags::IndexTransfer, Flags::DeviceLocal}}, {false}},
    BufferSpec{"depth-indices", BufferSlot::depth_indices, {SizeKind::compact_indices},
               {true, 0, {Flags::IndexTransfer, Flags::DeviceLocal}}, kDummy4},
    BufferSpec{"occluder-indices", BufferSlot::occluder_indices, {SizeKind::compact_indices},
               {true, 0, {Flags::IndexTransfer, Flags::DeviceLocal}}, kDummy4},
    // MID command lists are filled on the GPU; CID fills them on the CPU, so the CID side is host-visible.
    BufferSpec{"main-commands", BufferSlot::main_commands,
               {SizeKind::command_list, kIndirectCommandBytes},
               {false}, {true, 0, {Flags::IndirectTransfer, Flags::DeviceLocal}}},
    BufferSpec{"depth-commands", BufferSlot::depth_commands,
               {SizeKind::command_list, kIndirectCommandBytes},
               {true, 0, {Flags::IndirectTransfer, Flags::HostVisible}},
               {true, 0, {Flags::IndirectTransfer, Flags::DeviceLocal}}},
    BufferSpec{"occluder-commands", BufferSlot::occluder_commands,
               {SizeKind::command_list, kIndirectCommandBytes},
               {true, 0, {Flags::IndirectTransfer, Flags::HostVisible}},
               {true, 0, {Flags::IndirectTransfer, Flags::DeviceLocal}}},
    BufferSpec{"technique-region-bases", BufferSlot::technique_region_bases,
               {SizeKind::technique_counts},
               {true, 0, {Flags::StorageTransfer, Flags::HostVisible}},
               {true, 0, {Flags::StorageTransfer, Flags::HostVisible}}},
    BufferSpec{"depth-out-counter", BufferSlot::depth_out_counter, {SizeKind::counter},
               {true, 0, {Flags::StorageTransfer, Flags::HostVisible}},
               {true, 0, {Flags::StorageTransfer, Flags::HostVisible}}},
    // In MID the per-group count tables are the drawIndirectCount buffers.
    BufferSpec{"depth-technique-counts", BufferSlot::depth_technique_counts,
               {SizeKind::technique_counts},
               {true, 0, {Flags::StorageTransfer, Flags::HostVisible}},
               {true, 0, {Flags::IndirectTransfer, Flags::HostVisible}}},
    BufferSpec{"occluder-out-counter", BufferSlot::occluder_out_counter, {SizeKind::counter},
               {true, 0, {Flags::StorageTransfer, Flags::HostVisible}},
               {true, 0, {Flags::StorageTransfer, Flags::HostVisible}}},
    BufferSpec{"occluder-technique-counts", BufferSlot::occluder_technique_counts,
               {SizeKind::technique_counts},
               {true, 0, {Flags::StorageTransfer, Flags::HostVisible}},
               {true, 0, {Flags::IndirectTransfer, Flags::HostVisible}}},
    // technique_draw_commands is CID main pass only: MID keeps its commands in main_commands, so the CID-only collect-write set has nothing to bind.
    BufferSpec{"technique-draw-cmds", BufferSlot::technique_draw_commands,
               {SizeKind::technique_commands},
               {true, 0, {Flags::IndirectTransfer, Flags::HostVisible}}, {false}},
    BufferSpec{"occluder-candidate-count", BufferSlot::occluder_candidate_count,
               {SizeKind::counter},
               {true, 0, {Flags::StorageTransfer, Flags::HostVisible}},
               {true, 0, {Flags::StorageTransfer, Flags::HostVisible}}},
    BufferSpec{"technique-counts", BufferSlot::technique_counts, {SizeKind::technique_counts},
               {true, 0, {Flags::StorageTransfer, Flags::HostVisible}},
               {true, 0, {Flags::IndirectTransfer, Flags::HostVisible}}},
    BufferSpec{"technique-results", BufferSlot::technique_results,
               {SizeKind::technique_results, kTechniqueResultFloor},
               {true, 0, {Flags::StorageTransfer, Flags::HostVisible}},
               {true, 0, {Flags::StorageTransfer, Flags::HostVisible}}},
    BufferSpec{"bone-palettes", BufferSlot::bone_palettes, {SizeKind::bone_palettes},
               {true, 0, {Flags::StorageTransfer, Flags::HostVisible}},
               {true, 0, {Flags::StorageTransfer, Flags::HostVisible}}},
    BufferSpec{"modifier-params", BufferSlot::modifier_params, {SizeKind::modifier_params},
               {true, 0, {Flags::StorageTransfer, Flags::HostVisible}},
               {true, 0, {Flags::StorageTransfer, Flags::HostVisible}}},
    // Deform output: device-local, plus transfer-src so diagnostics and the GPU tests can copy a result back.
    BufferSpec{"deform-vertices", BufferSlot::deform_vertices, {SizeKind::deform_vertices},
               {true, 0, {Flags::StorageTransferReadable, Flags::DeviceLocal}},
               {true, 0, {Flags::StorageTransferReadable, Flags::DeviceLocal}}},
};

static_assert(kFrameBuffers.size() + 2 == kBufferSlotCount,
              "kFrameBuffers must hold one row per per-frame BufferSlot; the two "
              "singleton slots are tabled separately in kSingletonBuffers");

// Buffers that outlive the frame ring, owned by the renderer itself.
struct SingletonBufferSpec {
    BufferSlot slot = BufferSlot::technique_flags;
    SizeRule size{};
    BufferFlags flags{};
};

inline constexpr std::array kSingletonBuffers{
    // PipelineFlags table: one packed word per draw group, uploaded only on change.
    // Shared across frames on purpose (see SceneRenderer.cppm).
    SingletonBufferSpec{BufferSlot::technique_flags, {SizeKind::technique_counts},
                        {Flags::StorageTransfer, Flags::HostVisible}},
    // SceneHeader: the lighting uniform block, device-local, rewritten per frame through the staging pool.
    SingletonBufferSpec{BufferSlot::scene_header, {SizeKind::scene_header},
                        {Flags::StorageTransfer, Flags::DeviceLocal}},
};

// The row for a slot, or nullptr when the slot is not a singleton.
[[nodiscard]] constexpr const SingletonBufferSpec*
SingletonSpecOf(const BufferSlot slot) noexcept {
    for (const SingletonBufferSpec& spec : kSingletonBuffers) {
        if (spec.slot == slot) return &spec;
    }
    return nullptr;
}

// ── Per-frame block arrays ──
//
// One row per per-submesh block array.
// Every array is bounded by max_blocks_, because each is bound as a max_blocks_-sized descriptor array.

struct BlockArraySpec {
    BlockSlot slot = BlockSlot::dynamic_entries;
    std::uint32_t entry_size = 0;
    std::uint32_t entries_per_block = kEntriesPerBlock;
    vk::BufferUsageFlags extra_usage{};
    BufMemory memory = BufMemory::host_visible;
};

inline constexpr std::array kBlockArrays{
    // Entry sizes are the slab sizes gather writes; the mirrors live in SceneRenderer.cppm (VertexEntry, CullEntry, BoundingSphereEntry, ObbEntry, SkinEntry) and are pinned there by static_assert.
    BlockArraySpec{BlockSlot::dynamic_entries, 48},
    BlockArraySpec{BlockSlot::static_entries, 36},
    BlockArraySpec{BlockSlot::bounding_spheres, 16},
    BlockArraySpec{BlockSlot::obb_entries, 64},
    // Expand writes these two on the GPU; transfer-src is for diagnostics and the GPU tests that seed and read them back.
    BlockArraySpec{BlockSlot::submesh_vertex_entries, 192, kEntriesPerBlock,
                   vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst,
                   BufMemory::device_local},
    BlockArraySpec{BlockSlot::cull_entries, 16, kEntriesPerBlock,
                   vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst,
                   BufMemory::device_local},
    BlockArraySpec{BlockSlot::skin_entries, kSkinEntryBytes},
};

static_assert(kBlockArrays.size() == kBlockSlotCount,
              "kBlockArrays must hold one row per BlockSlot");

} // namespace VulkanEngine::SceneGpuLayout
