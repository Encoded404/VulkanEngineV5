// The shape of every GPU resource the scene renderer owns, as data.
//
// One row per descriptor set, one per binding, one per descriptor array the upload paths fill, one per pipeline layout, one per per-frame buffer and one per per-submesh block array.
// Layout creation, pool sizing, descriptor writes, buffer sizing, the set order of every pipeline layout and the per-stage descriptor-budget check all read these rows, so a count cannot be declared in one place and budgeted in another.
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

// Returned by the lookups below when the row they were asked for does not exist.
inline constexpr std::uint32_t kNoBinding = 0xFFFFFFFFu;

// A storage of one slot kind, indexed by the slot enum.
// The array size is the enum's own `count`, so a storage can never be shorter than the slots it serves, and the accessor is total: there is no "unknown slot" case to switch on.
template <typename T, typename Slot>
struct SlotArray {
    static_assert(std::is_enum_v<Slot>, "SlotArray needs a scoped enum with a `count` enumerator");

    std::array<T, static_cast<std::size_t>(Slot::count)> slots{};

    [[nodiscard]] constexpr T& operator[](const Slot slot) noexcept {
        return slots[static_cast<std::size_t>(slot)];
    }
    [[nodiscard]] constexpr const T& operator[](const Slot slot) const noexcept {
        return slots[static_cast<std::size_t>(slot)];
    }
};

// ── Slot identities ──
//
// SetId names a descriptor *layout*, not a set index: a pipeline that binds it picks the index (see kPipelineLayouts).
// The order matters only in that kSceneSets is indexed by SetId; a static_assert below enforces it.

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

// ── Descriptor sources ──
//
// A binding names the engine object that fills it, so the descriptor-write pass is a loop over the same rows that built the layout.
// The variant replaces what used to be a source enum plus three fields with sentinel defaults: a buffer row can no longer be read as a block row, because there is nothing to read.

struct BlockArraySource {
    BlockSlot slot = BlockSlot::dynamic_entries;
};

// `mid` equals `cid` for a binding that binds the same buffer in both draw modes.
struct BufferSource {
    BufferSlot cid = BufferSlot::draw_indices;
    BufferSlot mid = BufferSlot::draw_indices;
};

// A sampler and an image-view array, rewritten by the Hi-Z ring whenever it is re-created.
struct HizImageSource {};

// Filled out of band (mesh upload, lighting upload, Hi-Z re-create) or an empty placeholder.
// The frame loop never claims one of these.
struct FilledElsewhere {};

using BindingSource = std::variant<FilledElsewhere, BlockArraySource, BufferSource, HizImageSource>;

// Who writes a binding's descriptors.
// PrepareCompute only re-points `prepare_compute` rows; the rest belong to Initialize, the mesh upload path, the Hi-Z ring or the lighting upload.
// The default is `external` so a row that says nothing is left alone, which is the harmless direction.
enum class Writer : std::uint8_t { prepare_compute, external };

struct BindingSpec {
    std::uint32_t binding = 0;
    vk::DescriptorType kind = vk::DescriptorType::eStorageBuffer;
    // Descriptor count; empty means the runtime block budget (max_blocks_).
    std::optional<std::uint32_t> count{};
    vk::ShaderStageFlags stages = vk::ShaderStageFlagBits::eCompute;
    vk::DescriptorBindingFlags flags{};
    BindingSource source{};
    Writer writer = Writer::external;
};

// Row constructors, so the tables read as one line per binding.
// The argument order is the same in all four: binding, stages, then the payload, then the flags the row needs.
// The field order stays in the struct so a writer can still spell a row out longhand.
[[nodiscard]] constexpr BindingSpec Block(
    const std::uint32_t binding, const vk::ShaderStageFlags stages, const BlockSlot slot,
    const vk::DescriptorBindingFlags flags = {}) noexcept {
    return BindingSpec{.binding = binding,
                       .count = std::nullopt,
                       .stages = stages,
                       .flags = flags,
                       .source = BlockArraySource{slot},
                       .writer = Writer::prepare_compute};
}

[[nodiscard]] constexpr BindingSpec Buf(
    const std::uint32_t binding, const vk::ShaderStageFlags stages, const BufferSlot slot,
    const vk::DescriptorBindingFlags flags = {},
    const Writer writer = Writer::prepare_compute) noexcept {
    return BindingSpec{.binding = binding,
                       .count = 1,
                       .stages = stages,
                       .flags = flags,
                       .source = BufferSource{slot, slot},
                       .writer = writer};
}

[[nodiscard]] constexpr BindingSpec ModeBuf(
    const std::uint32_t binding, const vk::ShaderStageFlags stages,
    const BufferSlot cid_slot, const BufferSlot mid_slot,
    const vk::DescriptorBindingFlags flags = {}) noexcept {
    return BindingSpec{.binding = binding,
                       .count = 1,
                       .stages = stages,
                       .flags = flags,
                       .source = BufferSource{cid_slot, mid_slot},
                       .writer = Writer::prepare_compute};
}

// A binding with a descriptor array (a fixed table, the light table or a Hi-Z array); `kind` distinguishes sampled image, sampler, storage image and storage buffer.
[[nodiscard]] constexpr BindingSpec Array(
    const std::uint32_t binding, const vk::ShaderStageFlags stages,
    const vk::DescriptorType kind, const std::uint32_t count,
    const vk::DescriptorBindingFlags flags = {},
    const BindingSource source = FilledElsewhere{},
    const Writer writer = Writer::external) noexcept {
    return BindingSpec{.binding = binding,
                       .kind = kind,
                       .count = count,
                       .stages = stages,
                       .flags = flags,
                       .source = source,
                       .writer = writer};
}

struct SetSpec {
    SetId id = SetId::count;
    std::string_view name{};                 // "<name>-layout", "<name>-pool"
    std::span<const BindingSpec> bindings{};
    // Non-zero replaces frames_in_flight_ as the pool's maxSets, and the row then owns the one set allocated from that pool instead of the per-frame ring.
    std::uint32_t fixed_sets = 0;
    // The set is never bound in MID, where the buffers its bindings name do not exist, so the frame loop leaves those descriptors alone.
    bool cid_only = false;
};

// ── The tables ──
//
// One row per descriptor set, one row per binding inside it.
// Binding numbers, descriptor counts, stage masks, binding flags and the pool budget all come from these rows.

namespace Stages {
inline constexpr vk::ShaderStageFlags Vertex = vk::ShaderStageFlagBits::eVertex;
inline constexpr vk::ShaderStageFlags Compute = vk::ShaderStageFlagBits::eCompute;
inline constexpr vk::ShaderStageFlags VertexFragment =
    vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;
inline constexpr vk::ShaderStageFlags VertexCompute =
    vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eCompute;
} // namespace Stages

namespace BindFlags {
inline constexpr vk::DescriptorBindingFlags PartiallyBound =
    vk::DescriptorBindingFlagBits::ePartiallyBound;
inline constexpr vk::DescriptorBindingFlags UpdateAfterBind =
    vk::DescriptorBindingFlagBits::eUpdateAfterBind;
inline constexpr vk::DescriptorBindingFlags PartialUpdate =
    vk::DescriptorBindingFlagBits::ePartiallyBound |
    vk::DescriptorBindingFlagBits::eUpdateAfterBind;
inline constexpr vk::DescriptorBindingFlags VariableUpdate =
    vk::DescriptorBindingFlagBits::ePartiallyBound |
    vk::DescriptorBindingFlagBits::eUpdateAfterBind |
    vk::DescriptorBindingFlagBits::eVariableDescriptorCount;
} // namespace BindFlags

namespace Bindings {

using enum vk::DescriptorType;
using namespace Stages;
using namespace BindFlags;

// Set: submesh vertex entries. A plain block array with no update-after-bind flag, because gather only ever appends blocks and the descriptor is written before the set is bound.
inline constexpr std::array kSubmeshVertex{
    Block(0, VertexCompute, BlockSlot::submesh_vertex_entries),
};

// Set: vertex buffer table (binding 0), out-of-line UV table (binding 1) and the per-frame deformed-vertex buffer (binding 2).
// The two tables are fixed count: the spec allows the variable-count flag only on the highest binding, and neither array's runtime length is ever queried.
inline constexpr std::array kVertexBuffers{
    Array(0, VertexCompute, eStorageBuffer, kMaxVertexBuffers, PartialUpdate),
    Array(1, VertexCompute, eStorageBuffer, kMaxUvBuffers, PartialUpdate),
    Buf(2, Vertex, BufferSlot::deform_vertices),
};

// Vertex indirection. Update-after-bind so the frame loop can re-point it without a layout change.
inline constexpr std::array kIndirection{
    Buf(0, Vertex, BufferSlot::vertex_indirection, UpdateAfterBind),
};

// Index buffer table (bindless, update-after-bind) for expand, one set per frame.
inline constexpr std::array kIndexBuffers{
    Array(0, Compute, eStorageBuffer, kMaxIndexBuffers, VariableUpdate),
};

// Deform input set (set 1 of the deform pipeline): skin-influence table, extra influence table, flat bone palette, per-submesh SkinEntry block array, deformed-vertex output and the flat modifier parameters.
//
// The two skin tables are fixed descriptor arrays with the partially-bound and update-after-bind flags, because a mesh upload publishes a newly allocated skin block from the middle of the frame loop while earlier frames may still be reading the set.
inline constexpr std::array kDeform{
    Array(0, Compute, eStorageBuffer, kMaxSkinBuffers, PartialUpdate),
    Array(1, Compute, eStorageBuffer, kMaxSkinBuffers, PartialUpdate),
    Buf(2, Compute, BufferSlot::bone_palettes),
    Block(3, Compute, BlockSlot::skin_entries),
    Buf(4, Compute, BufferSlot::deform_vertices),
    Buf(5, Compute, BufferSlot::modifier_params),
};

// Expand. Five block arrays plus vertex_indirection, draw_indices and the expand counter.
// The fifth block array (the per-submesh SkinEntry table) sits at binding 7 so the three single bindings keep the numbers the shader already uses.
inline constexpr std::array kExpand{
    Block(0, Compute, BlockSlot::dynamic_entries),
    Block(1, Compute, BlockSlot::static_entries),
    Block(2, Compute, BlockSlot::submesh_vertex_entries),
    Block(3, Compute, BlockSlot::cull_entries),
    Buf(4, Compute, BufferSlot::vertex_indirection),
    Buf(5, Compute, BufferSlot::draw_indices),
    Buf(6, Compute, BufferSlot::expand_counter),
    Block(7, Compute, BlockSlot::skin_entries),
};

// Occlusion. Four block arrays, the Hi-Z combined image sampler, and the pre-cull survivor compaction bindings 6-11 that the pre-cull pass adds.
inline constexpr std::array kOcclusion{
    Block(0, Compute, BlockSlot::submesh_vertex_entries),
    Block(1, Compute, BlockSlot::cull_entries),
    Block(2, Compute, BlockSlot::bounding_spheres),
    Array(3, Compute, eCombinedImageSampler, 1, {}, HizImageSource{}, Writer::prepare_compute),
    Block(4, Compute, BlockSlot::obb_entries),
    Buf(5, Compute, BufferSlot::technique_flags),
    Buf(6, Compute, BufferSlot::draw_indices),
    Buf(7, Compute, BufferSlot::depth_commands),
    Buf(8, Compute, BufferSlot::depth_out_counter),
    Buf(9, Compute, BufferSlot::technique_region_bases),
    Buf(10, Compute, BufferSlot::depth_technique_counts),
    Buf(11, Compute, BufferSlot::depth_indices),
};

// Occluder select. Cull/vertex/OBB blocks plus the flag table, the full indirection, the occluder commands, three counters, the region bases, the per-group counts and the occluder index list.
inline constexpr std::array kOccluderSelect{
    Block(0, Compute, BlockSlot::cull_entries),
    Block(1, Compute, BlockSlot::submesh_vertex_entries),
    Block(2, Compute, BlockSlot::obb_entries),
    Buf(3, Compute, BufferSlot::technique_flags),
    Buf(4, Compute, BufferSlot::draw_indices),
    Buf(5, Compute, BufferSlot::occluder_commands),
    Buf(6, Compute, BufferSlot::occluder_out_counter),
    Buf(7, Compute, BufferSlot::occluder_candidate_count),
    Buf(8, Compute, BufferSlot::technique_region_bases),
    Buf(9, Compute, BufferSlot::occluder_technique_counts),
    Buf(10, Compute, BufferSlot::occluder_indices),
};

// Collect count/compact. One cull block array, the draw indices, the mode-dependent output (MID main commands, CID compact indices), the technique results, the region bases and the per-group counts.
inline constexpr std::array kCollect{
    Block(0, Compute, BlockSlot::cull_entries),
    Buf(1, Compute, BufferSlot::draw_indices),
    ModeBuf(2, Compute, BufferSlot::main_indices, BufferSlot::main_commands),
    Buf(3, Compute, BufferSlot::technique_results),
    Buf(4, Compute, BufferSlot::technique_region_bases),
    Buf(5, Compute, BufferSlot::technique_counts),
};

// Collect write, CID only. Emits one indexed indirect command per technique from the technique results.
inline constexpr std::array kCollectWrite{
    Buf(0, Compute, BufferSlot::technique_results),
    Buf(1, Compute, BufferSlot::technique_draw_commands),
};

// The engine-side lighting set: scene header and the Light block table.
inline constexpr std::array kLighting{
    // The header buffer is created once at initialize and never re-created, so the frame loop writes this descriptor once per epoch and never again.
    Buf(0, VertexFragment, BufferSlot::scene_header),
    Array(1, VertexFragment, eStorageBuffer, kMaxLightBlocks, PartiallyBound),
};

// Hi-Z generator: depth input, its sampler, and the storage-image array of mip levels. All three are rewritten by the Hi-Z ring on every re-create.
inline constexpr std::array kHiZ{
    Array(0, Compute, eSampledImage, 1, {}, HizImageSource{}),
    Array(1, Compute, eSampler, 1, {}, HizImageSource{}),
    Array(2, Compute, eStorageImage, kMaxHiZMips, {}, HizImageSource{}),
};

} // namespace Bindings

inline constexpr std::array kSceneSets{
    SetSpec{.id = SetId::submesh_vertex, .name = "submesh-vertex", .bindings = Bindings::kSubmeshVertex},
    SetSpec{.id = SetId::vertex_buffers, .name = "vertex-buffers", .bindings = Bindings::kVertexBuffers},
    SetSpec{.id = SetId::indirection, .name = "indirection", .bindings = Bindings::kIndirection},
    SetSpec{.id = SetId::index_buffers, .name = "index-buffers", .bindings = Bindings::kIndexBuffers},
    SetSpec{.id = SetId::deform, .name = "deform", .bindings = Bindings::kDeform},
    SetSpec{.id = SetId::expand, .name = "expand", .bindings = Bindings::kExpand},
    SetSpec{.id = SetId::occlusion, .name = "occlusion", .bindings = Bindings::kOcclusion},
    SetSpec{.id = SetId::occluder_select, .name = "occluder-select", .bindings = Bindings::kOccluderSelect},
    SetSpec{.id = SetId::collect, .name = "collect", .bindings = Bindings::kCollect},
    SetSpec{.id = SetId::collect_write, .name = "collect-write", .bindings = Bindings::kCollectWrite, .cid_only = true},
    // The empty set is a placeholder for set 0 of the depth pipeline.
    // It has no bindings, so its pool carries no descriptor types either.
    SetSpec{.id = SetId::empty, .name = "empty", .bindings = {}},
    SetSpec{.id = SetId::scene_uniform, .name = "scene-uniform", .bindings = Bindings::kLighting, .fixed_sets = 1},
    SetSpec{.id = SetId::hiz, .name = "hiz", .bindings = Bindings::kHiZ},
};

// ── Derivations ──

[[nodiscard]] constexpr const SetSpec& SetOf(const SetId id) noexcept {
    return kSceneSets[static_cast<std::size_t>(id)];
}

// Number of descriptors of one kind a set needs at `blocks` blocks.
[[nodiscard]] constexpr std::uint32_t CountOf(const SetId id, const vk::DescriptorType kind,
                                              const std::uint32_t blocks) noexcept {
    std::uint32_t n = 0;
    for (const BindingSpec& b : SetOf(id).bindings) {
        if (b.kind != kind) continue;
        n += b.count.value_or(blocks);
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
        if (!b.count) {
            ++fit.blocks;
        } else {
            fit.fixed += *b.count;
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

// A layout that carries any update-after-bind binding MUST be created with eUpdateAfterBindPool, and its pool with eUpdateAfterBind.
// Deriving that from the rows keeps the pool flag, the layout flag and the binding flags from disagreeing.
[[nodiscard]] constexpr bool UpdatesAfterBind(const SetId id) noexcept {
    for (const BindingSpec& b : SetOf(id).bindings) {
        if (static_cast<bool>(b.flags & vk::DescriptorBindingFlagBits::eUpdateAfterBind)) {
            return true;
        }
    }
    return false;
}

// The binding row that uses eVariableDescriptorCount, or nullptr.
// The spec allows at most one, and only on the highest binding of the set.
[[nodiscard]] constexpr const BindingSpec* VariableRow(const SetId id) noexcept {
    for (const BindingSpec& b : SetOf(id).bindings) {
        if (static_cast<bool>(b.flags & vk::DescriptorBindingFlagBits::eVariableDescriptorCount)) {
            return &b;
        }
    }
    return nullptr;
}

// The number of that binding, or kNoBinding.
[[nodiscard]] constexpr std::uint32_t VariableBinding(const SetId id) noexcept {
    const BindingSpec* row = VariableRow(id);
    return row != nullptr ? row->binding : kNoBinding;
}

// The descriptor count the layout declares for a binding: the block budget for a block-budget row, the literal otherwise.
[[nodiscard]] constexpr std::uint32_t DescriptorCount(const BindingSpec& binding,
                                                      const std::uint32_t blocks) noexcept {
    return binding.count.value_or(blocks);
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

// ── Descriptor arrays the upload paths fill ──
//
// The mesh upload, the lighting upload and the tests publish one element of a fixed descriptor array, and the shader hard-codes which binding that array lives in.
// Naming the array here lets the CPU side resolve the number from the rows instead of repeating a literal at every call site.

enum class TableId : std::uint8_t {
    vertex,        // vertex buffer table (block-growth model)
    uv,            // out-of-line UV table
    index,         // bindless index table
    skin,          // skin-influence table
    skin_extra,    // second skin-influence table
    light,         // Light block table of the lighting set
    count,
};

inline constexpr std::size_t kTableCount = static_cast<std::size_t>(TableId::count);

struct TableSpec {
    TableId id = TableId::count;
    SetId set = SetId::count;
    // Position of the storage-buffer binding inside the set; resolved through BindingOfKind so the number stays in the set row.
    std::uint32_t nth = 0;
};

inline constexpr std::array kTables{
    TableSpec{TableId::vertex, SetId::vertex_buffers, 0},
    TableSpec{TableId::uv, SetId::vertex_buffers, 1},
    TableSpec{TableId::index, SetId::index_buffers, 0},
    TableSpec{TableId::skin, SetId::deform, 0},
    TableSpec{TableId::skin_extra, SetId::deform, 1},
    TableSpec{TableId::light, SetId::scene_uniform, 1},
};

[[nodiscard]] constexpr const TableSpec& TableOf(const TableId id) noexcept {
    return kTables[static_cast<std::size_t>(id)];
}

// The binding a table writes, or nullptr when the row is missing.
[[nodiscard]] constexpr const BindingSpec* TableBinding(const TableId id) noexcept {
    const TableSpec& table = TableOf(id);
    const std::uint32_t number =
        BindingOfKind(table.set, vk::DescriptorType::eStorageBuffer, table.nth);
    if (number == kNoBinding) return nullptr;
    for (const BindingSpec& b : SetOf(table.set).bindings) {
        if (b.binding == number) return &b;
    }
    return nullptr;
}

// The binding number a table writes, or kNoBinding.
[[nodiscard]] constexpr std::uint32_t TableBindingNumber(const TableId id) noexcept {
    const TableSpec& table = TableOf(id);
    return BindingOfKind(table.set, vk::DescriptorType::eStorageBuffer, table.nth);
}

// Descriptor count of a table's binding, i.e. the largest array element an upload may write.
[[nodiscard]] constexpr std::uint32_t TableCapacity(const TableId id) noexcept {
    const BindingSpec* row = TableBinding(id);
    return row != nullptr ? row->count.value_or(0) : 0;
}

// ── Descriptor pool budget ──

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

// ── Descriptor-write cursors ──
//
// Every binding the frame loop writes owns a cursor holding the epoch it was last written in.
// A buffer or image binding is written once per epoch; a block array additionally writes only the blocks that do not exist yet.
// The flat index into the cursor array is the row's position among the writeable rows, so the cursor set cannot drift from the tables.

[[nodiscard]] constexpr bool IsFrameWritten(const BindingSpec& binding) noexcept {
    return binding.writer == Writer::prepare_compute;
}

[[nodiscard]] constexpr std::uint32_t WrittenBindingCount() noexcept {
    std::uint32_t n = 0;
    for (const SetSpec& set : kSceneSets) {
        for (const BindingSpec& b : set.bindings) {
            if (IsFrameWritten(b)) ++n;
        }
    }
    return n;
}

[[nodiscard]] constexpr std::uint32_t WrittenBindingIndex(const SetId id,
                                                          const std::uint32_t binding) noexcept {
    std::uint32_t index = 0;
    for (const SetSpec& set : kSceneSets) {
        for (const BindingSpec& b : set.bindings) {
            if (!IsFrameWritten(b)) continue;
            if (set.id == id && b.binding == binding) return index;
            ++index;
        }
    }
    return kNoBinding;
}

// Every block-array binding owns its own descriptor-write cursor.
// The same BlockArray is bound into several sets, and a single shared cursor would mark the frame's whole block budget written after the first pair, leaving every later binding with a descriptor that was never written (a GPU hang, not a wrong pixel).
[[nodiscard]] constexpr std::uint32_t BlockBindingCount() noexcept {
    std::uint32_t n = 0;
    for (const SetSpec& set : kSceneSets) {
        for (const BindingSpec& b : set.bindings) {
            if (std::holds_alternative<BlockArraySource>(b.source)) ++n;
        }
    }
    return n;
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

// ── Pipeline layouts ──
//
// A descriptor layout can be bound at any set index, so the index lives here rather than on the set row: this table is the only place a pipeline's set order is written down.
// `push_constant_bytes` is pinned by a static_assert next to each push-constant struct in the renderer.

enum class PipelineLayoutId : std::uint8_t {
    expand,
    occlusion,
    occluder_select,
    pre_cull,
    collect_count,
    collect_write,
    hiz,
    deform,
    depth,
    count,
};

inline constexpr std::size_t kPipelineLayoutCount =
    static_cast<std::size_t>(PipelineLayoutId::count);

namespace LayoutSets {
inline constexpr std::array<SetId, 2> expand{SetId::expand, SetId::index_buffers};
inline constexpr std::array<SetId, 1> occlusion{SetId::occlusion};
inline constexpr std::array<SetId, 1> occluder_select{SetId::occluder_select};
inline constexpr std::array<SetId, 1> collect{SetId::collect};
inline constexpr std::array<SetId, 1> collect_write{SetId::collect_write};
inline constexpr std::array<SetId, 1> hiz{SetId::hiz};
// Set 0 is the shared vertex-buffer table (widened to compute so the skinning stage can read the undeformed source vertices), set 1 is the deform input set.
inline constexpr std::array<SetId, 2> deform{SetId::vertex_buffers, SetId::deform};
// The depth prepass binds the empty placeholder at 0, then the three sets the main pass also binds.
inline constexpr std::array<SetId, 4> depth{
    SetId::empty, SetId::submesh_vertex, SetId::vertex_buffers, SetId::indirection};
} // namespace LayoutSets

struct PipelineLayoutSpec {
    PipelineLayoutId id = PipelineLayoutId::count;
    std::string_view name{};
    std::span<const SetId> sets{};
    std::uint32_t push_constant_bytes = 0;
};

inline constexpr std::array kPipelineLayouts{
    PipelineLayoutSpec{.id = PipelineLayoutId::expand, .name = "expand-pipeline-layout",
                       .sets = LayoutSets::expand, .push_constant_bytes = 84},
    PipelineLayoutSpec{.id = PipelineLayoutId::occlusion, .name = "occlusion-pipeline-layout",
                       .sets = LayoutSets::occlusion, .push_constant_bytes = 32},
    PipelineLayoutSpec{.id = PipelineLayoutId::occluder_select,
                       .name = "occluder-select-pipeline-layout",
                       .sets = LayoutSets::occluder_select, .push_constant_bytes = 16},
    // Pre-cull shares the occlusion set (widened with the survivor compaction bindings).
    PipelineLayoutSpec{.id = PipelineLayoutId::pre_cull, .name = "pre-cull-pipeline-layout",
                       .sets = LayoutSets::occlusion, .push_constant_bytes = 32},
    PipelineLayoutSpec{.id = PipelineLayoutId::collect_count,
                       .name = "collect-count-pipeline-layout",
                       .sets = LayoutSets::collect, .push_constant_bytes = 16},
    PipelineLayoutSpec{.id = PipelineLayoutId::collect_write,
                       .name = "collect-write-pipeline-layout",
                       .sets = LayoutSets::collect_write, .push_constant_bytes = 16},
    PipelineLayoutSpec{.id = PipelineLayoutId::hiz, .name = "hiz-gen-pipeline-layout",
                       .sets = LayoutSets::hiz, .push_constant_bytes = 16},
    // Every modifier pipeline shares the deform layout, so the two sets stay bound across vkCmdBindPipeline.
    PipelineLayoutSpec{.id = PipelineLayoutId::deform, .name = "deform-pipeline-layout",
                       .sets = LayoutSets::deform, .push_constant_bytes = 32},
    // The depth pipeline takes no push constants: the camera push belongs to the technique coverage pipelines, which own their layouts.
    PipelineLayoutSpec{.id = PipelineLayoutId::depth, .name = "depth-prepass-pipeline-layout",
                       .sets = LayoutSets::depth, .push_constant_bytes = 0},
};

[[nodiscard]] constexpr const PipelineLayoutSpec& LayoutOf(const PipelineLayoutId id) noexcept {
    return kPipelineLayouts[static_cast<std::size_t>(id)];
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

// When the frame loop zeroes a buffer before recording.
// The shaders append into these buffers with atomics, so last frame's contents would be read as this frame's counts.
enum class ClearMode : std::uint8_t { none, every_frame, cid_only };

struct BufferSpec {
    std::string_view name{};   // debug object name; "-dummy" is appended for a placeholder
    BufferSlot slot = BufferSlot::draw_indices;
    SizeRule size{};
    ModeSpec cid{};
    ModeSpec mid{};
    ClearMode clear = ClearMode::none;
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

namespace Modes {
// The combinations a row actually uses, so a row states a mode instead of a brace-initialized triple.
inline constexpr ModeSpec HostStorage{true, 0, {Flags::StorageTransfer, Flags::HostVisible}};
inline constexpr ModeSpec DeviceStorage{true, 0, {Flags::StorageTransfer, Flags::DeviceLocal}};
inline constexpr ModeSpec DeviceIndex{true, 0, {Flags::IndexTransfer, Flags::DeviceLocal}};
inline constexpr ModeSpec HostIndirect{true, 0, {Flags::IndirectTransfer, Flags::HostVisible}};
inline constexpr ModeSpec DeviceIndirect{true, 0, {Flags::IndirectTransfer, Flags::DeviceLocal}};
// Device-local plus transfer-src, so diagnostics and the GPU tests can copy a result back.
inline constexpr ModeSpec DeviceReadable{true, 0, {Flags::StorageTransferReadable, Flags::DeviceLocal}};
// The mode has no buffer for this slot; the binding is left unwritten (and, for a cid_only set, never bound).
inline constexpr ModeSpec Absent{false};
// A placeholder binding: present, fixed size, storage-only usage, device-local.
inline constexpr ModeSpec Dummy4{true, 4, {Flags::StorageTransfer, Flags::DeviceLocal}};
inline constexpr ModeSpec Dummy8{true, 8, {Flags::StorageTransfer, Flags::DeviceLocal}};
} // namespace Modes

// A buffer that is the same in both draw modes.
[[nodiscard]] constexpr BufferSpec Both(const std::string_view name, const BufferSlot slot,
                                        const SizeRule size, const ModeSpec mode,
                                        const ClearMode clear = ClearMode::none) noexcept {
    return BufferSpec{name, slot, size, mode, mode, clear};
}

// A command list must keep room for at least one command, so its floor is a record size rather than one byte.
// Every other row's formula already yields a usable size, so the default floor of 1 is all it needs.
inline constexpr std::uint64_t kTechniqueResultFloor = 8;

inline constexpr std::array kFrameBuffers{
    // vertex_indirection: CID carries one 8 B entry per vertex-window slot; MID binds a fixed 8 B dummy because it addresses vertices directly.
    BufferSpec{.name = "vertex-indirection", .slot = BufferSlot::vertex_indirection,
               .size = {SizeKind::vertex_indirection},
               .cid = Modes::DeviceStorage, .mid = Modes::Dummy8},
    // draw_indices is bound as an index buffer only in MID, where it holds the absolute vertex index of each occurrence.
    // CID binds the compacted main/depth/occluder lists instead, and only ever writes this one through a storage descriptor, so the index usage bit belongs to the MID variant.
    BufferSpec{.name = "draw-indices", .slot = BufferSlot::draw_indices,
               .size = {SizeKind::draw_indices},
               .cid = Modes::DeviceStorage, .mid = Modes::DeviceIndex},
    Both("expand-counter", BufferSlot::expand_counter, {SizeKind::expand_counter},
         Modes::HostStorage, ClearMode::every_frame),
    // CID compact index lists. MID specializes the writing shader path out, so the descriptors are 4 B dummies (depth/occluder) or absent (main).
    BufferSpec{.name = "main-indices", .slot = BufferSlot::main_indices,
               .size = {SizeKind::compact_indices},
               .cid = Modes::DeviceIndex, .mid = Modes::Absent},
    BufferSpec{.name = "depth-indices", .slot = BufferSlot::depth_indices,
               .size = {SizeKind::compact_indices},
               .cid = Modes::DeviceIndex, .mid = Modes::Dummy4},
    BufferSpec{.name = "occluder-indices", .slot = BufferSlot::occluder_indices,
               .size = {SizeKind::compact_indices},
               .cid = Modes::DeviceIndex, .mid = Modes::Dummy4},
    // MID command lists are filled on the GPU; CID fills them on the CPU, so the CID side is host-visible.
    BufferSpec{.name = "main-commands", .slot = BufferSlot::main_commands,
               .size = {SizeKind::command_list, kIndirectCommandBytes},
               .cid = Modes::Absent, .mid = Modes::DeviceIndirect},
    // CID fills the command list on the CPU (host-visible); MID fills it on the GPU (device-local), so the two modes differ in memory and not just in usage.
    BufferSpec{.name = "depth-commands", .slot = BufferSlot::depth_commands,
               .size = {SizeKind::command_list, kIndirectCommandBytes},
               .cid = Modes::HostIndirect, .mid = Modes::DeviceIndirect,
               .clear = ClearMode::cid_only},
    BufferSpec{.name = "occluder-commands", .slot = BufferSlot::occluder_commands,
               .size = {SizeKind::command_list, kIndirectCommandBytes},
               .cid = Modes::HostIndirect, .mid = Modes::DeviceIndirect,
               .clear = ClearMode::cid_only},
    Both("technique-region-bases", BufferSlot::technique_region_bases,
         {SizeKind::technique_counts}, Modes::HostStorage),
    Both("depth-out-counter", BufferSlot::depth_out_counter, {SizeKind::counter},
         Modes::HostStorage, ClearMode::every_frame),
    // In MID the per-group count tables are the drawIndirectCount buffers.
    // The shaders append into them through InterlockedAdd in both modes, so they are cleared every frame like technique-counts.
    BufferSpec{.name = "depth-technique-counts", .slot = BufferSlot::depth_technique_counts,
               .size = {SizeKind::technique_counts},
               .cid = Modes::HostStorage, .mid = Modes::HostIndirect,
               .clear = ClearMode::every_frame},
    Both("occluder-out-counter", BufferSlot::occluder_out_counter, {SizeKind::counter},
         Modes::HostStorage, ClearMode::every_frame),
    BufferSpec{.name = "occluder-technique-counts", .slot = BufferSlot::occluder_technique_counts,
               .size = {SizeKind::technique_counts},
               .cid = Modes::HostStorage, .mid = Modes::HostIndirect,
               .clear = ClearMode::every_frame},
    // technique_draw_commands is CID main pass only: MID keeps its commands in main_commands, so the CID-only collect-write set has nothing to bind.
    BufferSpec{.name = "technique-draw-cmds", .slot = BufferSlot::technique_draw_commands,
               .size = {SizeKind::technique_commands},
               .cid = Modes::HostIndirect, .mid = Modes::Absent, .clear = ClearMode::cid_only},
    Both("occluder-candidate-count", BufferSlot::occluder_candidate_count,
         {SizeKind::counter}, Modes::HostStorage, ClearMode::every_frame),
    BufferSpec{.name = "technique-counts", .slot = BufferSlot::technique_counts,
               .size = {SizeKind::technique_counts},
               .cid = Modes::HostStorage, .mid = Modes::HostIndirect,
               .clear = ClearMode::every_frame},
    Both("technique-results", BufferSlot::technique_results,
         {SizeKind::technique_results, kTechniqueResultFloor}, Modes::HostStorage,
         ClearMode::every_frame),
    Both("bone-palettes", BufferSlot::bone_palettes, {SizeKind::bone_palettes},
         Modes::HostStorage),
    Both("modifier-params", BufferSlot::modifier_params, {SizeKind::modifier_params},
         Modes::HostStorage),
    // Deform output: device-local, plus transfer-src so the diagnostics and the GPU tests can copy a result back.
    Both("deform-vertices", BufferSlot::deform_vertices, {SizeKind::deform_vertices},
         Modes::DeviceReadable),
};

static_assert(kFrameBuffers.size() + 2 == kBufferSlotCount,
              "kFrameBuffers must hold one row per per-frame BufferSlot; the two "
              "singleton slots are tabled separately in kSingletonBuffers");

// Buffers that outlive the frame ring, owned by the renderer itself.
struct SingletonBufferSpec {
    BufferSlot slot = BufferSlot::technique_flags;
    std::string_view name{};   // debug object name
    SizeRule size{};
    BufferFlags flags{};
};

inline constexpr std::array kSingletonBuffers{
    // PipelineFlags table: one packed word per draw group, uploaded only on change.
    // Shared across frames on purpose (see SceneRenderer.cppm).
    SingletonBufferSpec{BufferSlot::technique_flags, "technique-flags",
                        {SizeKind::technique_counts},
                        {Flags::StorageTransfer, Flags::HostVisible}},
    // SceneHeader: the lighting header, device-local, rewritten per frame through the staging pool.
    SingletonBufferSpec{BufferSlot::scene_header, "scene-header", {SizeKind::scene_header},
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

// True for a slot that lives on the renderer rather than in the frame ring.
[[nodiscard]] constexpr bool IsSingleton(const BufferSlot slot) noexcept {
    return SingletonSpecOf(slot) != nullptr;
}

// ── Per-frame block arrays ──
//
// One row per per-submesh block array.
// Every array is bounded by max_blocks_, because each is bound as a max_blocks_-sized descriptor array.

// Which pass grows an array to the frame's submesh count.
// Gather owns the SkinEntry table (it writes one entry per submesh there); every other array is grown by the frame loop before the descriptor writes.
enum class GrownBy : std::uint8_t { frame_loop, gather };

struct BlockArraySpec {
    BlockSlot slot = BlockSlot::dynamic_entries;
    std::string_view name{};   // diagnostic name
    std::uint32_t entry_size = 0;
    std::uint32_t entries_per_block = kEntriesPerBlock;
    vk::BufferUsageFlags extra_usage{};
    BufMemory memory = BufMemory::host_visible;
    GrownBy grown_by = GrownBy::frame_loop;
};

inline constexpr std::array kBlockArrays{
    // Entry sizes are the slab sizes gather writes; the mirrors live in SceneRenderer.cppm (VertexEntry, CullEntry, BoundingSphereEntry, ObbEntry, SkinEntry) and are pinned there by static_assert.
    BlockArraySpec{BlockSlot::dynamic_entries, "dynamic_entries", 48},
    BlockArraySpec{BlockSlot::static_entries, "static_entries", 36},
    BlockArraySpec{BlockSlot::bounding_spheres, "bounding_spheres", 16},
    BlockArraySpec{BlockSlot::obb_entries, "obb_entries", 64},
    // Expand writes these two on the GPU; transfer-src is for diagnostics and the GPU tests that seed and read them back.
    BlockArraySpec{BlockSlot::submesh_vertex_entries, "submesh_vertex_entries", 192, kEntriesPerBlock,
                   vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst,
                   BufMemory::device_local},
    BlockArraySpec{BlockSlot::cull_entries, "cull_entries", 16, kEntriesPerBlock,
                   vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst,
                   BufMemory::device_local},
    // Gather writes one SkinEntry per submesh and owns the capacity of its own array.
    BlockArraySpec{BlockSlot::skin_entries, "skin_entries", kSkinEntryBytes, kEntriesPerBlock, {},
                   BufMemory::host_visible, GrownBy::gather},
};

static_assert(kBlockArrays.size() == kBlockSlotCount,
              "kBlockArrays must hold one row per BlockSlot");

// ── Table invariants ──
//
// These were tests. The rows are constexpr, so they belong at compile time: an edit that violates one cannot be built rather than merely reported.

[[nodiscard]] consteval bool BindingsAreUniquePerSet() {
    for (const SetSpec& set : kSceneSets) {
        for (std::size_t i = 0; i < set.bindings.size(); ++i) {
            for (std::size_t j = i + 1; j < set.bindings.size(); ++j) {
                if (set.bindings[i].binding == set.bindings[j].binding) return false;
            }
        }
    }
    return true;
}
static_assert(BindingsAreUniquePerSet(), "two bindings in one set share a binding number");

[[nodiscard]] consteval bool BlockBudgetBelongsToBlockArrays() {
    for (const SetSpec& set : kSceneSets) {
        for (const BindingSpec& b : set.bindings) {
            const bool takes_the_block_budget = !b.count.has_value();
            const bool binds_a_block_array = std::holds_alternative<BlockArraySource>(b.source);
            if (takes_the_block_budget != binds_a_block_array) return false;
        }
    }
    return true;
}
static_assert(BlockBudgetBelongsToBlockArrays(),
              "a block-budget binding is a block array and a block array is a block-budget binding");

[[nodiscard]] consteval bool UnclaimedSourcesAreLeftAlone() {
    for (const SetSpec& set : kSceneSets) {
        for (const BindingSpec& b : set.bindings) {
            if (std::holds_alternative<FilledElsewhere>(b.source) &&
                b.writer == Writer::prepare_compute) {
                return false;
            }
        }
    }
    return true;
}
static_assert(UnclaimedSourcesAreLeftAlone(),
              "a binding with no engine-side source cannot claim the frame-loop writer");

[[nodiscard]] consteval bool BlockSlotsAreTabledAndBound() {
    std::array<bool, kBlockSlotCount> tabled{};
    for (const BlockArraySpec& spec : kBlockArrays) {
        const auto slot = static_cast<std::size_t>(spec.slot);
        if (slot >= tabled.size() || tabled[slot] || spec.entry_size == 0) return false;
        tabled[slot] = true;
    }
    std::array<bool, kBlockSlotCount> bound{};
    for (const SetSpec& set : kSceneSets) {
        for (const BindingSpec& b : set.bindings) {
            if (const auto* source = std::get_if<BlockArraySource>(&b.source)) {
                bound[static_cast<std::size_t>(source->slot)] = true;
            }
        }
    }
    return tabled == bound;
}
static_assert(BlockSlotsAreTabledAndBound(),
              "every block slot has exactly one kBlockArrays row and is bound by exactly one set binding");

[[nodiscard]] consteval bool VariableCountIsOnTheHighestBinding() {
    for (const SetSpec& set : kSceneSets) {
        const std::uint32_t variable = VariableBinding(set.id);
        if (variable == kNoBinding) continue;
        if (variable + 1 != BindingCount(set.id)) return false;
    }
    return true;
}
static_assert(VariableCountIsOnTheHighestBinding(),
              "eVariableDescriptorCount is only legal on a set's highest binding");

[[nodiscard]] consteval bool UpdateAfterBindSetsAreKnown() {
    for (const SetSpec& set : kSceneSets) {
        const bool expected = set.id == SetId::vertex_buffers || set.id == SetId::indirection ||
                              set.id == SetId::index_buffers || set.id == SetId::deform;
        if (UpdatesAfterBind(set.id) != expected) return false;
    }
    return true;
}
static_assert(UpdateAfterBindSetsAreKnown(),
              "exactly the sets the mesh upload re-points mid-frame are update-after-bind");

[[nodiscard]] consteval bool TablesResolveToAStorageBinding() {
    for (std::size_t i = 0; i < kTables.size(); ++i) {
        if (static_cast<std::size_t>(kTables[i].id) != i) return false;
        const BindingSpec* row = TableBinding(kTables[i].id);
        if (row == nullptr || !row->count.has_value()) return false;
        if (!std::holds_alternative<FilledElsewhere>(row->source)) return false;
    }
    return true;
}
static_assert(TablesResolveToAStorageBinding(),
              "every table names a fixed-count storage-buffer binding that no frame-loop writer claims");

// The shaders hard-code these numbers, so the pairing is pinned here; a renumbering that keeps both sides in step passes, one that only touches the table fails to build.
static_assert(TableBindingNumber(TableId::vertex) == 0 &&
                  TableBindingNumber(TableId::uv) == 1 &&
                  TableBindingNumber(TableId::index) == 0 &&
                  TableBindingNumber(TableId::skin) == 0 &&
                  TableBindingNumber(TableId::skin_extra) == 1 &&
                  TableBindingNumber(TableId::light) == 1,
              "the upload paths write the binding numbers the scene shaders declare");

[[nodiscard]] consteval bool BufferRowsCoverEverySlot() {
    std::array<bool, kBufferSlotCount> tabled{};
    for (const BufferSpec& spec : kFrameBuffers) {
        const auto slot = static_cast<std::size_t>(spec.slot);
        if (slot >= tabled.size() || tabled[slot] || IsSingleton(spec.slot)) return false;
        if (!spec.cid.present && !spec.mid.present) return false;
        if (spec.cid.present && !spec.cid.flags.usage) return false;
        if (spec.mid.present && !spec.mid.flags.usage) return false;
        tabled[slot] = true;
    }
    for (const SingletonBufferSpec& spec : kSingletonBuffers) {
        const auto slot = static_cast<std::size_t>(spec.slot);
        if (slot >= tabled.size() || tabled[slot]) return false;
        tabled[slot] = true;
    }
    for (const bool present : tabled) {
        if (!present) return false;
    }
    return true;
}
static_assert(BufferRowsCoverEverySlot(),
              "kFrameBuffers and kSingletonBuffers together hold exactly one row per BufferSlot");

[[nodiscard]] consteval bool BindingBuffersAreTabled() {
    const auto tabled = [](const BufferSlot slot) {
        if (SingletonSpecOf(slot) != nullptr) return true;
        for (const BufferSpec& spec : kFrameBuffers) {
            if (spec.slot == slot) return true;
        }
        return false;
    };
    for (const SetSpec& set : kSceneSets) {
        for (const BindingSpec& b : set.bindings) {
            const auto* source = std::get_if<BufferSource>(&b.source);
            if (source == nullptr) continue;
            if (!tabled(source->cid) || !tabled(source->mid)) return false;
        }
    }
    return true;
}
static_assert(BindingBuffersAreTabled(), "every buffer binding names a tabled buffer slot");

[[nodiscard]] consteval bool ClearedBuffersAreKnown() {
    // The shaders append into these buffers with atomics (or, for the CID command lists, leave unused slots to be read as indexCount 0 by the prepass walk).
    // A buffer that accumulates across frames and is not cleared starts the frame at the previous frame's total, so the set is pinned: a row that gains a clear, or loses one, has to be reconsidered here.
    std::array<bool, kBufferSlotCount> cleared{};
    for (const BufferSpec& spec : kFrameBuffers) {
        if (spec.clear == ClearMode::none) continue;
        cleared[static_cast<std::size_t>(spec.slot)] = true;
    }
    for (std::size_t i = 0; i < cleared.size(); ++i) {
        const auto slot = static_cast<BufferSlot>(i);
        const bool expected = slot == BufferSlot::expand_counter ||
                              slot == BufferSlot::occluder_candidate_count ||
                              slot == BufferSlot::depth_out_counter ||
                              slot == BufferSlot::occluder_out_counter ||
                              slot == BufferSlot::depth_technique_counts ||
                              slot == BufferSlot::occluder_technique_counts ||
                              slot == BufferSlot::technique_counts ||
                              slot == BufferSlot::technique_results ||
                              // CID-only command lists: every unused slot must stay indexCount 0.
                              slot == BufferSlot::depth_commands ||
                              slot == BufferSlot::occluder_commands ||
                              slot == BufferSlot::technique_draw_commands;
        if (cleared[i] != expected) return false;
    }
    return true;
}
static_assert(ClearedBuffersAreKnown(),
              "the frame loop clears exactly the buffers that accumulate across frames");

[[nodiscard]] consteval bool SizeFloorsAreRecordsOrOneByte() {
    for (const BufferSpec& spec : kFrameBuffers) {
        if (spec.size.min_bytes != 1 && spec.size.min_bytes != kIndirectCommandBytes &&
            spec.size.min_bytes != kTechniqueResultFloor) {
            return false;
        }
        if (spec.cid.fixed_bytes != 0 && spec.cid.fixed_bytes < 4) return false;
        if (spec.mid.fixed_bytes != 0 && spec.mid.fixed_bytes < 4) return false;
    }
    for (const SingletonBufferSpec& spec : kSingletonBuffers) {
        if (spec.size.min_bytes != 1) return false;
    }
    return true;
}
static_assert(SizeFloorsAreRecordsOrOneByte(),
              "a size floor is one byte or the record size the formula can under-shoot");

[[nodiscard]] consteval bool EveryWriterValueIsUsed() {
    std::uint32_t frame_loop = 0;
    std::uint32_t out_of_band = 0;
    for (const SetSpec& set : kSceneSets) {
        for (const BindingSpec& b : set.bindings) {
            if (b.writer == Writer::prepare_compute) ++frame_loop;
            if (b.writer == Writer::external) ++out_of_band;
        }
    }
    return frame_loop > 0 && out_of_band > 0;
}
static_assert(EveryWriterValueIsUsed(), "a writer value no row names is a distinction nothing acts on");

[[nodiscard]] consteval bool ExactlyOneSetIsNotRinged() {
    std::uint32_t fixed_rows = 0;
    for (const SetSpec& set : kSceneSets) {
        if (set.fixed_sets == 0) continue;
        if (set.id != SetId::scene_uniform || set.fixed_sets != 1) return false;
        ++fixed_rows;
    }
    return fixed_rows == 1;
}
static_assert(ExactlyOneSetIsNotRinged(),
              "one set row owns its single set; a second would need the renderer to tell the two apart");

[[nodiscard]] consteval bool PipelineLayoutRowsAreWellFormed() {
    for (std::size_t i = 0; i < kPipelineLayouts.size(); ++i) {
        const PipelineLayoutSpec& layout = kPipelineLayouts[i];
        if (static_cast<std::size_t>(layout.id) != i || layout.sets.empty()) return false;
        for (std::size_t a = 0; a < layout.sets.size(); ++a) {
            for (std::size_t b = a + 1; b < layout.sets.size(); ++b) {
                if (layout.sets[a] == layout.sets[b]) return false;
            }
        }
        // A push-constant range is a multiple of 4 bytes, and the depth layout takes none.
        if (layout.push_constant_bytes % 4 != 0) return false;
        if ((layout.id == PipelineLayoutId::depth) != (layout.push_constant_bytes == 0)) return false;
    }
    return true;
}
static_assert(PipelineLayoutRowsAreWellFormed(),
              "one row per PipelineLayoutId, in id order, with distinct sets and a 4-byte-aligned push range");

// The write cursor is indexed positionally, so an unknown row would silently share or lose a cursor.
static_assert(BlockBindingCount() == 15,
              "fifteen block-array bindings; the write cursors and docs/indexed-drawing-pipeline.md pin this");

[[nodiscard]] consteval bool WrittenBindingsAreIndexable() {
    for (const SetSpec& set : kSceneSets) {
        for (const BindingSpec& b : set.bindings) {
            if (!IsFrameWritten(b)) continue;
            if (WrittenBindingIndex(set.id, b.binding) == kNoBinding) return false;
        }
    }
    return true;
}
static_assert(WrittenBindingsAreIndexable(),
              "every frame-written row has a descriptor-write cursor");

} // namespace VulkanEngine::SceneGpuLayout
