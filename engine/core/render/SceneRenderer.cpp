module;

#include <cassert>

#include <logging/logging_macros.hpp>

module VulkanEngine.SceneRenderer;

import std;
import std.compat;

import logiface;

import vulkan;

import VulkanEngine.ECS.ComponentRegistry;
import VulkanBackend.Vulkan.VulkanBootstrap;
import VulkanBackend.Vulkan.MemoryUtils;
import VulkanBackend.Vulkan.VulkanDebugUtils;
import VulkanEngine.GpuResources;
import VulkanEngine.StandardMeshPipeline;
import VulkanEngine.SceneGpuLayout;
import VulkanEngine.SceneLimits;
import VulkanEngine.PipelineFactory;
import VulkanEngine.ShaderRegistration;
import VulkanBackend.Vulkan.VulkanCapabilities;

namespace VulkanEngine::SceneRenderer {

// SceneLimits is Vulkan-free, so its record sizes are plain integers; they are pinned to the types they describe here, where both are visible.
static_assert(sizeof(vk::DrawIndexedIndirectCommand) == SceneLimits::kIndirectCommandBytes,
              "SceneLimits::kIndirectCommandBytes must equal vk::DrawIndexedIndirectCommand");
static_assert(sizeof(SkinEntry) == SceneLimits::kSkinEntryBytes,
              "SceneLimits::kSkinEntryBytes must equal SkinEntry");

namespace {

// ── Table translation ──
//
// The rows already carry vk:: flag words, so the only translation left is the block-array memory kind, which has to produce a BlockArray::MemoryMode as well as a flag word.

constexpr vk::MemoryPropertyFlags ToVkMemory(const SceneGpuLayout::BufMemory memory) {
    return memory == SceneGpuLayout::BufMemory::host_visible
        ? vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent
        : vk::MemoryPropertyFlags(vk::MemoryPropertyFlagBits::eDeviceLocal);
}

// ── Binding numbers for the paths that write descriptors outside PrepareCompute ──
//
// The shaders hard-code these numbers, so the table is the only place the CPU side may name them.
// Looking them up by kind and position keeps a renumbering to one edit, and the static_asserts turn a missing row into a compile error instead of a write to kNoBinding.

inline constexpr std::uint32_t kVertexTableBinding = SceneGpuLayout::BindingOfKind(
    SceneGpuLayout::SetId::vertex_buffers, vk::DescriptorType::eStorageBuffer, 0);
inline constexpr std::uint32_t kUvTableBinding = SceneGpuLayout::BindingOfKind(
    SceneGpuLayout::SetId::vertex_buffers, vk::DescriptorType::eStorageBuffer, 1);
inline constexpr std::uint32_t kIndexTableBinding = SceneGpuLayout::BindingOfKind(
    SceneGpuLayout::SetId::index_buffers, vk::DescriptorType::eStorageBuffer, 0);
inline constexpr std::uint32_t kSkinTableBinding = SceneGpuLayout::BindingOfKind(
    SceneGpuLayout::SetId::deform, vk::DescriptorType::eStorageBuffer, 0);
inline constexpr std::uint32_t kSkinExtraTableBinding = SceneGpuLayout::BindingOfKind(
    SceneGpuLayout::SetId::deform, vk::DescriptorType::eStorageBuffer, 1);
inline constexpr std::uint32_t kLightTableBinding = SceneGpuLayout::BindingOfKind(
    SceneGpuLayout::SetId::scene_uniform, vk::DescriptorType::eStorageBuffer, 1);

static_assert(kVertexTableBinding != SceneGpuLayout::kNoBinding &&
              kUvTableBinding != SceneGpuLayout::kNoBinding &&
              kIndexTableBinding != SceneGpuLayout::kNoBinding &&
              kSkinTableBinding != SceneGpuLayout::kNoBinding &&
              kSkinExtraTableBinding != SceneGpuLayout::kNoBinding &&
              kLightTableBinding != SceneGpuLayout::kNoBinding,
              "the upload paths write these bindings, so the tables must keep the rows");

// The vk:: create info for one set, plus the storage its pNext chain points at.
// Build() must run once the object is in its final place: `info` keeps raw pointers into `bindings` and `binding_flags`.
struct LayoutPlan {
    std::vector<vk::DescriptorSetLayoutBinding> bindings;
    std::vector<vk::DescriptorBindingFlags> binding_flags;
    vk::DescriptorSetLayoutBindingFlagsCreateInfo flag_info{};
    vk::DescriptorSetLayoutCreateInfo info{};

    void Build(const SceneGpuLayout::SetSpec& set, const std::uint32_t max_blocks) {
        bindings.reserve(set.bindings.size());
        binding_flags.reserve(set.bindings.size());
        for (const SceneGpuLayout::BindingSpec& row : set.bindings) {
            vk::DescriptorSetLayoutBinding binding{};
            binding.binding = row.binding;
            binding.descriptorType = row.kind;
            binding.descriptorCount = SceneGpuLayout::DescriptorCount(row, max_blocks);
            binding.stageFlags = row.stages;
            bindings.push_back(binding);
            binding_flags.push_back(row.flags);
        }
        if (!bindings.empty()) {
            // pNext carries one flag word per binding; an all-zero word means no special behaviour, so this is safe for every set.
            flag_info.bindingCount = static_cast<std::uint32_t>(binding_flags.size());
            flag_info.pBindingFlags = binding_flags.data();
            info.pNext = &flag_info;
        }
        if (SceneGpuLayout::UpdatesAfterBind(set.id)) {
            // Required when any binding uses eUpdateAfterBind, and required on the pool the set is allocated from.
            // Deriving it from the rows keeps the two from disagreeing.
            info.flags = vk::DescriptorSetLayoutCreateFlagBits::eUpdateAfterBindPool;
        }
        info.bindingCount = static_cast<std::uint32_t>(bindings.size());
        info.pBindings = bindings.data();
    }
};

GpuResources::DescriptorPoolConfig ToPoolConfig(const SceneGpuLayout::SetSpec& set,
                                                const SceneGpuLayout::PoolCounts& counts) {
    GpuResources::DescriptorPoolConfig config{};
    config.max_sets = counts.max_sets;
    config.max_storage_buffers = counts.storage_buffers;
    config.max_combined_image_samplers = counts.combined_image_samplers;
    config.max_sampled_images = counts.sampled_images;
    config.max_storage_images = counts.storage_images;
    config.max_samplers = counts.samplers;
    // A layout created with eUpdateAfterBindPool requires a pool with the same flag, and both must agree with the binding flags.
    config.update_after_bind = SceneGpuLayout::UpdatesAfterBind(set.id);
    return config;
}

} // anonymous namespace

SceneRenderer::~SceneRenderer() {
    Shutdown();
}

// ── FrameResources slot accessors ──
//
// One switch per slot kind keeps the tables free of member pointers (and of this private struct).
// The cost is three small functions; the benefit is that a missing enum value fails to compile here rather than becoming a silent null handle.

GpuResources::GpuDescriptorSet& SceneRenderer::FrameResources::SetFor(
    const SceneGpuLayout::SetId id) {
    switch (id) {
        case SceneGpuLayout::SetId::expand: return expand_set;
        case SceneGpuLayout::SetId::occlusion: return occlusion_set;
        case SceneGpuLayout::SetId::collect: return collect_set;
        case SceneGpuLayout::SetId::collect_write: return collect_write_set;
        case SceneGpuLayout::SetId::occluder_select: return occluder_select_set;
        case SceneGpuLayout::SetId::submesh_vertex: return submesh_vertex_set;
        case SceneGpuLayout::SetId::indirection: return indirection_raw_set;
        case SceneGpuLayout::SetId::vertex_buffers: return vertex_buffers_set;
        case SceneGpuLayout::SetId::index_buffers: return index_buffers_set;
        case SceneGpuLayout::SetId::deform: return deform_set;
        case SceneGpuLayout::SetId::hiz: return hiz_set;
        case SceneGpuLayout::SetId::empty: return empty_set;
        case SceneGpuLayout::SetId::scene_uniform: break;   // owned by the renderer
        case SceneGpuLayout::SetId::count: break;
    }
    // The lighting set is a single set on the renderer, not part of the ring.
    // There is no valid handle to return: a misbind here is exactly the failure the tables exist to prevent, so it traps instead of binding the wrong set.
    assert(id != SceneGpuLayout::SetId::scene_uniform &&
           "SetFor(SetId::scene_uniform) is not part of the frame ring");
    std::unreachable();
}

const GpuResources::GpuDescriptorSet& SceneRenderer::FrameResources::SetFor(
    const SceneGpuLayout::SetId id) const {
    return const_cast<FrameResources*>(this)->SetFor(id);
}

GpuResources::GpuBuffer& SceneRenderer::FrameResources::BufferFor(
    const SceneGpuLayout::BufferSlot slot) {
    switch (slot) {
        case SceneGpuLayout::BufferSlot::vertex_indirection: return vertex_indirection;
        case SceneGpuLayout::BufferSlot::draw_indices: return draw_indices;
        case SceneGpuLayout::BufferSlot::expand_counter: return expand_counter;
        case SceneGpuLayout::BufferSlot::main_indices: return main_indices;
        case SceneGpuLayout::BufferSlot::depth_indices: return depth_indices;
        case SceneGpuLayout::BufferSlot::occluder_indices: return occluder_indices;
        case SceneGpuLayout::BufferSlot::main_commands: return main_commands;
        case SceneGpuLayout::BufferSlot::depth_commands: return depth_commands;
        case SceneGpuLayout::BufferSlot::occluder_commands: return occluder_commands;
        case SceneGpuLayout::BufferSlot::technique_region_bases: return technique_region_bases;
        case SceneGpuLayout::BufferSlot::depth_out_counter: return depth_out_counter;
        case SceneGpuLayout::BufferSlot::depth_technique_counts: return depth_technique_counts;
        case SceneGpuLayout::BufferSlot::occluder_out_counter: return occluder_out_counter;
        case SceneGpuLayout::BufferSlot::occluder_technique_counts: return occluder_technique_counts;
        case SceneGpuLayout::BufferSlot::technique_draw_commands: return technique_draw_commands;
        case SceneGpuLayout::BufferSlot::occluder_candidate_count: return occluder_candidate_count;
        case SceneGpuLayout::BufferSlot::technique_counts: return technique_counts;
        case SceneGpuLayout::BufferSlot::technique_results: return technique_results;
        case SceneGpuLayout::BufferSlot::bone_palettes: return bone_palettes;
        case SceneGpuLayout::BufferSlot::modifier_params: return modifier_params;
        case SceneGpuLayout::BufferSlot::deform_vertices: return deform_vertices;
        case SceneGpuLayout::BufferSlot::technique_flags:
        case SceneGpuLayout::BufferSlot::scene_header:
            break;   // singletons: see SceneRenderer::BufferRef
        case SceneGpuLayout::BufferSlot::count: break;
    }
    assert(slot != SceneGpuLayout::BufferSlot::technique_flags &&
           slot != SceneGpuLayout::BufferSlot::scene_header &&
           "singleton buffers are not part of the frame ring");
    std::unreachable();
}

const GpuResources::GpuBuffer& SceneRenderer::FrameResources::BufferFor(
    const SceneGpuLayout::BufferSlot slot) const {
    return const_cast<FrameResources*>(this)->BufferFor(slot);
}

GpuResources::BlockArray& SceneRenderer::FrameResources::BlockArrayFor(
    const SceneGpuLayout::BlockSlot slot) {
    switch (slot) {
        case SceneGpuLayout::BlockSlot::dynamic_entries: return dynamic_entries;
        case SceneGpuLayout::BlockSlot::static_entries: return static_entries;
        case SceneGpuLayout::BlockSlot::bounding_spheres: return bounding_spheres;
        case SceneGpuLayout::BlockSlot::obb_entries: return obb_entries;
        case SceneGpuLayout::BlockSlot::submesh_vertex_entries: return submesh_vertex_entries;
        case SceneGpuLayout::BlockSlot::cull_entries: return cull_entries;
        case SceneGpuLayout::BlockSlot::skin_entries: return skin_entries;
        case SceneGpuLayout::BlockSlot::count: break;
    }
    assert(false && "unknown BlockSlot");
    std::unreachable();
}

const GpuResources::BlockArray& SceneRenderer::FrameResources::BlockArrayFor(
    const SceneGpuLayout::BlockSlot slot) const {
    return const_cast<FrameResources*>(this)->BlockArrayFor(slot);
}

GpuResources::GpuBuffer& SceneRenderer::BufferRef(FrameResources& fr,
                                                  const SceneGpuLayout::BufferSlot slot) {
    if (slot == SceneGpuLayout::BufferSlot::technique_flags) return technique_flags;
    if (slot == SceneGpuLayout::BufferSlot::scene_header) return scene_header;
    return fr.BufferFor(slot);
}

vk::DescriptorSetLayout SceneRenderer::GetSetLayout(const SceneGpuLayout::SetId id) const {
    const SetHandles& slot = sets_[static_cast<std::size_t>(id)];
    return slot.layout ? static_cast<vk::DescriptorSetLayout>(**slot.layout)
                       : vk::DescriptorSetLayout{};
}

std::uint64_t SceneRenderer::ComputeSize(const SceneGpuLayout::SizeKind kind) const {
    const auto& capacity = scene_capacity_;
    switch (kind) {
        // One 8 B entry per slot in each submesh's tight vertex window.
        case SceneGpuLayout::SizeKind::vertex_indirection:
            return static_cast<std::uint64_t>(capacity.vertex_span) * 8u;
        // One 4 B index/vertex slot per index occurrence.
        case SceneGpuLayout::SizeKind::draw_indices:
        case SceneGpuLayout::SizeKind::compact_indices:
            return static_cast<std::uint64_t>(capacity.index_count) * 4u;
        // One indirect command per submesh occurrence.
        case SceneGpuLayout::SizeKind::command_list:
            return static_cast<std::uint64_t>(capacity.submesh_count) *
                   SceneLimits::kIndirectCommandBytes;
        case SceneGpuLayout::SizeKind::technique_counts:
            return static_cast<std::uint64_t>(MAX_DRAW_GROUPS) * sizeof(std::uint32_t);
        case SceneGpuLayout::SizeKind::technique_commands:
            return static_cast<std::uint64_t>(MAX_DRAW_GROUPS) *
                   SceneLimits::kIndirectCommandBytes;
        case SceneGpuLayout::SizeKind::technique_results:
            return static_cast<std::uint64_t>(MAX_DRAW_GROUPS) * sizeof(TechniqueResult);
        case SceneGpuLayout::SizeKind::counter:
            return sizeof(std::uint32_t);
        // [0] entry base, [1] index base, [2] deform-region base.
        // The third word is expand's per-frame deform allocator.
        case SceneGpuLayout::SizeKind::expand_counter:
            return 3u * sizeof(std::uint32_t);
        // One mat4 per palette matrix.
        case SceneGpuLayout::SizeKind::bone_palettes:
            return static_cast<std::uint64_t>(capacity.palette_matrix_count) *
                   16u * sizeof(float);
        case SceneGpuLayout::SizeKind::modifier_params:
            return static_cast<std::uint64_t>(capacity.modifier_param_bytes);
        // Two regions of T vertices each: the modifier stack writes A, B, A..., and the final region is what the vertex shaders read.
        case SceneGpuLayout::SizeKind::deform_vertices:
            return static_cast<std::uint64_t>(2u * capacity.deform_vertex_span) *
                   sizeof(VulkanEngine::StandardMeshPipeline::Vertex);
        case SceneGpuLayout::SizeKind::scene_header:
            return sizeof(SceneHeader);
    }
    std::unreachable();
}

std::uint64_t SceneRenderer::BufferBytes(const SceneGpuLayout::SizeRule& rule) const {
    // The row owns the floor, so this only applies it: the formula is raw, and a zero-capacity scene is kept off the zero-byte allocation by the default floor of 1.
    return std::max<std::uint64_t>(ComputeSize(rule.kind), rule.min_bytes);
}

bool SceneRenderer::CreateDescriptorSets(
    GpuResources::DeviceBufferHeap& vh) {
    if (!backend_) return false;
    auto& be = *backend_;
    const auto& dev = be.GetDevice();

    for (const SceneGpuLayout::SetSpec& set : SceneGpuLayout::kSceneSets) {
        SetHandles& slot = SetSlot(set.id);
        LayoutPlan plan;
        plan.Build(set, max_blocks_);
        slot.layout = std::make_unique<vk::raii::DescriptorSetLayout>(dev, plan.info);
        VulkanBackend::Vulkan::SetVulkanObjectName(
            dev, *slot.layout, std::string(set.name) + "-layout");
        slot.pool = GpuResources::DescriptorPool::Create(
            be, ToPoolConfig(set, SceneGpuLayout::PoolCountsFor(set.id, max_blocks_,
                                                             frames_in_flight_)));
        slot.pool->SetDebugName(dev, std::string(set.name) + "-pool");
    }

    // One allocation shape for both kinds of row: a variable-count set (the bindless index table) is allocated at its full binding count, so the shader's array length matches the descriptor array.
    const auto allocate = [this](const SceneGpuLayout::SetSpec& set) {
        SetHandles& slot = SetSlot(set.id);
        const std::uint32_t variable = SceneGpuLayout::VariableBinding(set.id);
        if (variable == SceneGpuLayout::kNoBinding) {
            return slot.pool->Allocate(**slot.layout);
        }
        // VariableBinding only returns a binding number it found in this set, so the row is here.
        for (const SceneGpuLayout::BindingSpec& row : set.bindings) {
            if (row.binding == variable) {
                return slot.pool->Allocate(**slot.layout,
                                           SceneGpuLayout::DescriptorCount(row, max_blocks_));
            }
        }
        std::unreachable();
    };

    // A fixed row owns one set, allocated here from the pool its `fixed_sets` sized.
    // The field now has one mechanism behind it instead of a flag plus a special case in this function.
    for (const SceneGpuLayout::SetSpec& set : SceneGpuLayout::kSceneSets) {
        if (set.fixed_sets == 0) continue;
        VulkanEngine::GpuResources::GpuDescriptorSet& owned =
            owned_sets_[static_cast<std::size_t>(set.id)];
        owned = allocate(set);
        owned.SetDebugName(dev, std::string(set.name) + "-set");
    }

    // Every other row gets one set per frame in flight.
    for (auto& fr : frames_) {
        for (const SceneGpuLayout::SetSpec& set : SceneGpuLayout::kSceneSets) {
            if (set.fixed_sets != 0) continue;
            fr.SetFor(set.id) = allocate(set);
            fr.SetFor(set.id).SetDebugName(dev, std::string(set.name) + "-set");
        }
    }

    // Publish the blocks the vertex heap already holds.
    // Every later block is written when it is allocated (UpdateVertexBufferArrayElement), but a heap populated before the renderer existed has no such event, so the initial descriptors are written here.
    for (auto& fr : frames_) {
        for (std::uint32_t block = 0; block < vh.GetBufferCount(); ++block) {
            const vk::DescriptorBufferInfo info(vh.GetBuffer(block), 0, vk::WholeSize);
            vk::WriteDescriptorSet write{};
            write.dstSet = fr.vertex_buffers_set.GetHandle();
            write.dstBinding = kVertexTableBinding;
            write.dstArrayElement = block;
            write.descriptorCount = 1;
            write.descriptorType = vk::DescriptorType::eStorageBuffer;
            write.pBufferInfo = &info;
            dev.updateDescriptorSets(write, nullptr);
        }
    }

    return true;
}

void SceneRenderer::WriteBinding(FrameResources& fr, const SceneGpuLayout::SetSpec& set,
                                 const SceneGpuLayout::BindingSpec& binding) {
    if (!backend_) return;
    auto& dev = backend_->GetDevice();
    const vk::DescriptorSet dst = fr.SetFor(set.id).GetHandle();
    if (!dst) return;

    switch (binding.source) {
        case SceneGpuLayout::SourceKind::block_array: {
            GpuResources::BlockArray& blocks = fr.BlockArrayFor(binding.block_slot);
            const std::uint32_t cursor =
                SceneGpuLayout::BlockBindingIndex(set.id, binding.binding);
            if (cursor == SceneGpuLayout::kNoBinding) return;
            auto& cache = fr.block_binding_cache[cursor];
            if (cache.epoch != binding_epoch_) {
                cache.written_blocks = 0;
                cache.epoch = binding_epoch_;
            }
            // Only the blocks that do not exist yet: blocks are appended and never removed, and binding_epoch_ forces a full rewrite after a capacity change re-created the buffers.
            for (std::uint32_t block = cache.written_blocks; block < blocks.BlockCount();
                 ++block) {
                const vk::DescriptorBufferInfo info(blocks.GetBlockArray(block), 0,
                                                    blocks.BlockSize());
                vk::WriteDescriptorSet write{};
                write.dstSet = dst;
                write.dstBinding = binding.binding;
                write.dstArrayElement = block;
                write.descriptorCount = 1;
                write.descriptorType = binding.kind;
                write.pBufferInfo = &info;
                dev.updateDescriptorSets(write, nullptr);
            }
            cache.written_blocks = blocks.BlockCount();
            break;
        }
        case SceneGpuLayout::SourceKind::buffer: {
            GpuResources::GpuBuffer& buffer = BufferRef(fr, binding.buffer_slot);
            if (!buffer.IsValid()) return;
            const vk::DescriptorBufferInfo info(*buffer.GetBuffer(), 0, vk::WholeSize);
            vk::WriteDescriptorSet write{};
            write.dstSet = dst;
            write.dstBinding = binding.binding;
            write.descriptorCount = 1;
            write.descriptorType = binding.kind;
            write.pBufferInfo = &info;
            dev.updateDescriptorSets(write, nullptr);
            break;
        }
        case SceneGpuLayout::SourceKind::mode_buffer: {
            const SceneGpuLayout::BufferSlot slot = draw_mode_ == DrawMode::MID
                ? binding.buffer_slot_mid
                : binding.buffer_slot;
            GpuResources::GpuBuffer& buffer = BufferRef(fr, slot);
            if (!buffer.IsValid()) return;
            const vk::DescriptorBufferInfo info(*buffer.GetBuffer(), 0, vk::WholeSize);
            vk::WriteDescriptorSet write{};
            write.dstSet = dst;
            write.dstBinding = binding.binding;
            write.descriptorCount = 1;
            write.descriptorType = binding.kind;
            write.pBufferInfo = &info;
            dev.updateDescriptorSets(write, nullptr);
            break;
        }
        case SceneGpuLayout::SourceKind::hiz_image: {
            // The occlusion set's combined image sampler.
            // Both handles come from the Hi-Z ring, so a headless frame (which never created it) leaves the binding alone.
            if (!hiz_sampler_ || !*fr.hiz_full_view) return;
            const vk::DescriptorImageInfo info(**hiz_sampler_, *fr.hiz_full_view,
                                               vk::ImageLayout::eShaderReadOnlyOptimal);
            vk::WriteDescriptorSet write{};
            write.dstSet = dst;
            write.dstBinding = binding.binding;
            write.descriptorCount = 1;
            write.descriptorType = binding.kind;
            write.pImageInfo = &info;
            dev.updateDescriptorSets(write, nullptr);
            break;
        }
        case SceneGpuLayout::SourceKind::none:
            break;
    }
}

void SceneRenderer::WriteHiZBindings(FrameResources& fr) {
    if (!backend_ || !hiz_sampler_ || !*fr.hiz_image || hiz_mip_count_ == 0) return;
    auto& dev = backend_->GetDevice();
    const vk::DescriptorSet dst = fr.hiz_set.GetHandle();
    if (!dst) return;

    const std::uint32_t depth_binding = SceneGpuLayout::BindingOfKind(
        SceneGpuLayout::SetId::hiz, vk::DescriptorType::eSampledImage);
    const std::uint32_t sampler_binding = SceneGpuLayout::BindingOfKind(
        SceneGpuLayout::SetId::hiz, vk::DescriptorType::eSampler);
    const std::uint32_t levels_binding = SceneGpuLayout::BindingOfKind(
        SceneGpuLayout::SetId::hiz, vk::DescriptorType::eStorageImage);
    assert(depth_binding != SceneGpuLayout::kNoBinding &&
           sampler_binding != SceneGpuLayout::kNoBinding &&
           levels_binding != SceneGpuLayout::kNoBinding);

    // Bind the whole storage-image array the layout declares (descriptorCount = MAX_HIZ_MIPS).
    // Re-pointing the tail levels (beyond mip_levels) at the coarsest mip keeps every descriptor valid after a shrink; leaving them would dangle at the previous resize's views.
    std::vector<vk::DescriptorImageInfo> storage_infos;
    storage_infos.reserve(MAX_HIZ_MIPS);
    const std::uint32_t last_level = hiz_mip_count_ - 1;
    for (std::uint32_t mip = 0; mip < MAX_HIZ_MIPS; ++mip) {
        const std::uint32_t view = std::min(mip, last_level);
        storage_infos.emplace_back(nullptr, *fr.hiz_mip_views[view],
                                   vk::ImageLayout::eGeneral);
    }
    {
        vk::WriteDescriptorSet write{};
        write.dstSet = dst;
        write.dstBinding = levels_binding;
        write.dstArrayElement = 0;
        write.descriptorCount = MAX_HIZ_MIPS;
        write.descriptorType = vk::DescriptorType::eStorageImage;
        write.pImageInfo = storage_infos.data();
        dev.updateDescriptorSets(write, nullptr);
    }
    // Bind hiz mip 0 as the placeholder depth input; eGeneral matches the storage-image binding on the same subresource.
    {
        const vk::DescriptorImageInfo info(nullptr, *fr.hiz_mip_views[0],
                                           vk::ImageLayout::eGeneral);
        vk::WriteDescriptorSet write{};
        write.dstSet = dst;
        write.dstBinding = depth_binding;
        write.descriptorCount = 1;
        write.descriptorType = vk::DescriptorType::eSampledImage;
        write.pImageInfo = &info;
        dev.updateDescriptorSets(write, nullptr);
    }
    {
        const vk::DescriptorImageInfo info(**hiz_sampler_, nullptr,
                                           vk::ImageLayout::eShaderReadOnlyOptimal);
        vk::WriteDescriptorSet write{};
        write.dstSet = dst;
        write.dstBinding = sampler_binding;
        write.descriptorCount = 1;
        write.descriptorType = vk::DescriptorType::eSampler;
        write.pImageInfo = &info;
        dev.updateDescriptorSets(write, nullptr);
    }
}

bool SceneRenderer::Initialize(VulkanBackend::Vulkan::IVulkanBootstrap& be,
                                VulkanEngine::GpuResources::DeviceBufferHeap& vh,
                                SceneCapacity initial_capacity,
                                ShaderSystem::ShaderManager& shader_mgr,
                                ShaderSystem::PipelineFactory& pipeline_factory,
                                const EngineShaderIds& shader_ids,
                                const std::uint32_t frames_in_flight,
                                DrawMode draw_mode,
                                Modifiers::ModifierManager& modifier_mgr) {
    backend_ = &be;
    shader_mgr_ = &shader_mgr;
    pipeline_factory_ = &pipeline_factory;
    modifier_mgr_ = &modifier_mgr;
    shader_ids_ = shader_ids;
    const auto& dev = be.GetDevice();
    draw_indirect_count_supported_ =
        be.GetCapabilities().IsFeatureEnabled(
            VulkanBackend::Vulkan::Feature::DrawIndirectCount);
    draw_indirect_first_instance_supported_ =
        be.GetCapabilities().IsFeatureEnabled(
            VulkanBackend::Vulkan::Feature::DrawIndirectFirstInstance);
    // MID needs both: the count-driven draw and a non-zero firstInstance
    // (the submesh id). Store the combined rule once so selection and
    // Reinitialize cannot disagree.
    mid_supported_ =
        draw_indirect_count_supported_ && draw_indirect_first_instance_supported_;
    draw_mode_ = ResolveDrawMode(draw_mode);
    if (draw_mode_ != draw_mode) {
        LOGIFACE_LOG(warn, "SceneRenderer: drawIndirectCount and drawIndirectFirstInstance "
                           "not both supported; falling back to CID draw mode");
    }
    // One line per run naming the shape actually used: the requested mode can be MID by default and still resolve to CID.
    // The effective mode is therefore not readable from the config alone.
    LOGIFACE_LOG(info, std::string("SceneRenderer: draw mode ") +
        (draw_mode_ == DrawMode::MID ? "MID" : "CID"));

    // Resolve the submesh-block budget from the device before any descriptor
    // layout is created: binding array sizes and descriptor pool sizes both
    // derive from it. Descriptor binding arrays are static, so the compile-time
    // ceiling bounds them and the device clamps below it.
    {
        const auto& limits = be.GetCapabilities().GetProperties().limits;
        const SceneGpuLayout::DeviceLimits device_limits{
            .max_descriptor_set_storage_buffers =
                limits.maxDescriptorSetStorageBuffers,
            .max_per_stage_descriptor_storage_buffers =
                limits.maxPerStageDescriptorStorageBuffers,
        };
        max_blocks_ = std::clamp(SceneGpuLayout::MaxBlocksForDevice(device_limits),
                                 1u, SceneLimits::kMaxBlocksCeiling);
        LOGIFACE_LOG(info, "SceneRenderer: submesh block budget max_blocks=" +
                     std::to_string(max_blocks_) + " (" + std::to_string(MaxSubmeshes()) +
                     " submeshes; ceiling " +
                     std::to_string(SceneLimits::kMaxBlocksCeiling) + ")");
        if (max_blocks_ < SceneLimits::kMaxBlocksCeiling) {
            LOGIFACE_LOG(info, "SceneRenderer: block budget clamped by device descriptor limits "
                         "(maxDescriptorSetStorageBuffers=" +
                         std::to_string(limits.maxDescriptorSetStorageBuffers) +
                         ", maxPerStageDescriptorStorageBuffers=" +
                         std::to_string(limits.maxPerStageDescriptorStorageBuffers) + ")");
        }
    }
    scene_capacity_ = SceneCapacity{
        .index_count = std::max(initial_capacity.index_count, 1u),
        .vertex_span = std::max(initial_capacity.vertex_span, 1u),
        .submesh_count = std::max(initial_capacity.submesh_count, 1u),
        .deform_vertex_span = initial_capacity.deform_vertex_span,
        .palette_matrix_count = initial_capacity.palette_matrix_count,
        // The parameter buffer's floor is a constant, not the (usually zero)
        // init-time estimate: growing it later device-idles in the middle of a
        // frame, and the first frame that carries a modifier should not pay that.
        .modifier_param_bytes = std::max(initial_capacity.modifier_param_bytes,
                                         SceneLimits::kInitialModifierParamBytes),
    };

    // Size the per-frame resource ring to the configured pipeline depth. All
    // descriptor pools and per-frame GPU resources scale with this count.
    frames_in_flight_ = std::max(frames_in_flight, 1u);
    frames_.resize(frames_in_flight_);
    frame_modifier_dispatches_.assign(frames_in_flight_, {});

    // Size every pipeline retire ring consistently with the frame depth.
    expand_slot_.SetFramesInFlight(frames_in_flight_);
    occlusion_slot_.SetFramesInFlight(frames_in_flight_);
    hiz_slot_.SetFramesInFlight(frames_in_flight_);
    collect_count_slot_.SetFramesInFlight(frames_in_flight_);
    collect_write_slot_.SetFramesInFlight(frames_in_flight_);
    depth_slot_.SetFramesInFlight(frames_in_flight_);
    occluder_select_slot_.SetFramesInFlight(frames_in_flight_);
    occluder_select_mid_slot_.SetFramesInFlight(frames_in_flight_);
    pre_cull_slot_.SetFramesInFlight(frames_in_flight_);
    pre_cull_mid_slot_.SetFramesInFlight(frames_in_flight_);
    collect_count_mid_slot_.SetFramesInFlight(frames_in_flight_);

    // Descriptor sets: layouts, pools and the per-frame sets.
    // Every binding number, descriptor count, stage mask, binding flag and pool size comes from SceneGpuLayout::kSceneSets, so a set's shape is declared exactly once.
    if (!CreateDescriptorSets(vh)) return false;

    // Create all compute/graphics pipelines in one batched submission
    if (!CreateScenePipelines(be, shader_mgr, pipeline_factory, shader_ids)) return false;

    // Per-frame ring resources.
    {
        // The shared technique flag table is a singleton, not part of the ring.
        const SceneGpuLayout::SingletonBufferSpec* flags_spec =
            SceneGpuLayout::SingletonSpecOf(SceneGpuLayout::BufferSlot::technique_flags);
        technique_flags = GpuResources::GpuBuffer::Create(be, BufferBytes(flags_spec->size),
            flags_spec->flags.usage, flags_spec->flags.memory);
        VulkanBackend::Vulkan::SetVulkanObjectName(dev, technique_flags.GetBuffer(),
                                                   vk::ObjectType::eBuffer, "technique-flags");

        // Per-submesh block arrays. Each row is bounded to the runtime block budget: its descriptors are a max_blocks_-sized binding array, so growing past that would write descriptors out of range.
        for (auto& fr : frames_) {
            for (const SceneGpuLayout::BlockArraySpec& spec : SceneGpuLayout::kBlockArrays) {
                GpuResources::BlockArray::Config cfg{};
                cfg.entry_size = spec.entry_size;
                cfg.entries_per_block = spec.entries_per_block;
                cfg.max_blocks = max_blocks_;
                cfg.extra_usage = spec.extra_usage;
                cfg.memory = ToVkMemory(spec.memory);
                // One field produces both: an array that is mapped must be host-visible, and one that is not takes the staging path in UploadEntry.
                cfg.memory_mode = spec.memory == SceneGpuLayout::BufMemory::device_local
                    ? GpuResources::MemoryMode::DeviceLocal
                    : GpuResources::MemoryMode::HostVisible;
                fr.BlockArrayFor(spec.slot).Initialize(be, cfg);
            }
        }

        if (!CreateFrameBuffers()) return false;
    }

    // Build any modifier pipelines that are already registered (the builtin
    // skinning entry is registered before this call). Gather on the first frame
    // needs IsModifierReady == true to emit the skinning stage.
    SyncModifierPipelines(0);

    (void)be.GetSwapchainExtent(depth_width_, depth_height_);

    // A headless bootstrap has no swapchain and leaves the extent at zero. Hi-Z
    // needs a positive extent (vkCreateImage rejects a zero width/height), so
    // defer the pyramid until EnsureRenderExtent supplies one. Tests and
    // embedders that only dispatch compute/cull work never need it; a windowed
    // app always reports a real extent here.
    if (depth_width_ > 0 && depth_height_ > 0) {
        if (!CreateHiZResources()) return false;
    }

    // ── Lighting system (descriptor set 4) ──
    //
    // The layout and pool come from the set table; only the two objects and the initial descriptor writes live here.
    {
        const SceneGpuLayout::SingletonBufferSpec* header_spec =
            SceneGpuLayout::SingletonSpecOf(SceneGpuLayout::BufferSlot::scene_header);
        scene_header = GpuResources::GpuBuffer::Create(be, BufferBytes(header_spec->size),
            header_spec->flags.usage, header_spec->flags.memory, nullptr);
        VulkanBackend::Vulkan::SetVulkanObjectName(dev, scene_header.GetBuffer(), "scene-header");

        const std::uint32_t header_binding = SceneGpuLayout::BindingOfKind(
            SceneGpuLayout::SetId::scene_uniform, vk::DescriptorType::eStorageBuffer, 0);
        static_assert(header_binding != SceneGpuLayout::kNoBinding,
                      "the lighting set must keep a storage-buffer header binding");
        const auto& header_buffer = scene_header.GetBuffer();
        const vk::DescriptorBufferInfo header_buf_info(
            static_cast<vk::Buffer>(*header_buffer), 0, sizeof(SceneHeader));
        vk::WriteDescriptorSet header_write{};
        header_write.dstSet = GetSceneUniformSet();
        header_write.dstBinding = header_binding;
        header_write.descriptorCount = 1;
        header_write.descriptorType = vk::DescriptorType::eStorageBuffer;
        header_write.pBufferInfo = &header_buf_info;
        dev.updateDescriptorSets(header_write, nullptr);

        // Initialize the light block array. Its descriptor binding is a fixed kMaxLightBlocks array, so the BlockArray must not grow past it.
        GpuResources::BlockArray::Config light_cfg{};
        light_cfg.entry_size = static_cast<std::uint32_t>(sizeof(Light));
        light_cfg.entries_per_block = LIGHTS_PER_BLOCK;
        light_cfg.max_blocks = MAX_LIGHT_BLOCKS;
        light_cfg.memory_mode = GpuResources::MemoryMode::DeviceLocal;
        light_cfg.memory = vk::MemoryPropertyFlagBits::eDeviceLocal;
        light_cfg.extra_usage = vk::BufferUsageFlagBits::eTransferDst;
        scene_lights.Initialize(be, light_cfg);
    }

    LOGIFACE_LOG(info, "SceneRenderer initialized");
    return true;
}

bool SceneRenderer::IsDrawModeSupported(DrawMode mode) const {
    switch (mode) {
        case DrawMode::CID: return true;
        case DrawMode::MID: return mid_supported_;
    }
    return false;
}

DrawMode SceneRenderer::ResolveDrawMode(DrawMode requested) const {
    if (requested == DrawMode::MID && !mid_supported_) {
        return DrawMode::CID;
    }
    return requested;
}

bool SceneRenderer::CreateFrameBuffers() {
    if (!backend_) return false;
    auto& be = *backend_;
    const auto& dev = be.GetDevice();

    // Any descriptor written against the previous buffers is stale; force a
    // full block-descriptor rewrite on the next PrepareCompute.
    ++binding_epoch_;

    const bool mid = draw_mode_ == DrawMode::MID;
    for (auto& fr : frames_) {
        for (const SceneGpuLayout::BufferSpec& spec : SceneGpuLayout::kFrameBuffers) {
            const SceneGpuLayout::ModeSpec& mode = mid ? spec.mid : spec.cid;
            if (!mode.present) continue;
            // A fixed size is a placeholder for a shader path this mode specializes out; otherwise the size derives from the capacity.
            const std::uint64_t bytes = mode.fixed_bytes != 0
                ? mode.fixed_bytes
                : BufferBytes(spec.size);
            GpuResources::GpuBuffer& buffer = fr.BufferFor(spec.slot);
            buffer = GpuResources::GpuBuffer::Create(be, bytes, mode.flags.usage,
                                                     mode.flags.memory);
            VulkanBackend::Vulkan::SetVulkanObjectName(
                dev, buffer.GetBuffer(), vk::ObjectType::eBuffer,
                std::string(spec.name) + (mode.fixed_bytes != 0 ? "-dummy" : ""));
        }
    }

    // CPU-prefix region bases for MID (recomputed when topology changes).
    region_base_.assign(MAX_DRAW_GROUPS, 0);
    region_count_.assign(MAX_DRAW_GROUPS, 0);
    region_total_ = 0;
    technique_count_ = 0;
    return true;
}

void SceneRenderer::DestroyFrameBuffers() {
    // Every row, not a hand-maintained list: a slot that gains a buffer cannot be forgotten here, and a mode that has no buffer for a slot just holds an already-invalid one.
    for (auto& fr : frames_) {
        for (const SceneGpuLayout::BufferSpec& spec : SceneGpuLayout::kFrameBuffers) {
            fr.BufferFor(spec.slot) = GpuResources::GpuBuffer{};
        }
    }
}

void SceneRenderer::DestroyHiZResources() {
    for (auto& fr : frames_) {
        // Views must outlive the image; the image must be destroyed before the
        // device memory it is bound to is freed.
        fr.hiz_mip_views.clear();
        fr.hiz_full_view = vk::raii::ImageView(nullptr);
        fr.hiz_image = vk::raii::Image(nullptr);
        fr.hiz_memory = vk::raii::DeviceMemory(nullptr);
    }
    hiz_sampler_.reset();
    hiz_mip_count_ = 0;
}

bool SceneRenderer::CreateHiZResources() {
    if (!backend_) return false;
    auto& be = *backend_;
    const auto& dev = be.GetDevice();

    const std::uint32_t hiz_w = (depth_width_ + 1) / 2;
    const std::uint32_t hiz_h = (depth_height_ + 1) / 2;
    std::uint32_t max_dim = std::max(hiz_w, hiz_h);
    std::uint32_t mip_levels = 1;
    while (max_dim > 1) { max_dim >>= 1; ++mip_levels; }
    mip_levels = std::min(mip_levels, MAX_HIZ_MIPS);
    const vk::Format hiz_format = vk::Format::eR32Sfloat;

    vk::SamplerCreateInfo sampler_ci{};
    sampler_ci.magFilter = vk::Filter::eNearest;
    sampler_ci.minFilter = vk::Filter::eNearest;
    sampler_ci.mipmapMode = vk::SamplerMipmapMode::eNearest;
    // Clamp instead of the default Repeat: out-of-range UVs (e.g. a sphere center
    // off-screen) must sample the edge texel, whose max depth is a conservative
    // proxy, never a wrapped texel from the opposite edge.
    sampler_ci.addressModeU = vk::SamplerAddressMode::eClampToEdge;
    sampler_ci.addressModeV = vk::SamplerAddressMode::eClampToEdge;
    sampler_ci.addressModeW = vk::SamplerAddressMode::eClampToEdge;
    sampler_ci.minLod = 0.0f;
    sampler_ci.maxLod = static_cast<float>(mip_levels);
    hiz_sampler_ = std::make_unique<vk::raii::Sampler>(dev, sampler_ci);
    VulkanBackend::Vulkan::SetVulkanObjectName(dev, *hiz_sampler_, "hiz-sampler");

    // The level count describes the ring being built, and WriteHiZBindings below already needs it to clamp the tail descriptors, so publish it before the frames are created rather than after.
    hiz_mip_count_ = mip_levels;

    for (auto& fr : frames_) {
        vk::ImageCreateInfo img_ci{};
        img_ci.imageType = vk::ImageType::e2D;
        img_ci.format = hiz_format;
        img_ci.extent = vk::Extent3D(hiz_w, hiz_h, 1);
        img_ci.mipLevels = mip_levels;
        img_ci.arrayLayers = 1;
        img_ci.samples = vk::SampleCountFlagBits::e1;
        img_ci.tiling = vk::ImageTiling::eOptimal;
        img_ci.usage = vk::ImageUsageFlagBits::eStorage |
                       vk::ImageUsageFlagBits::eSampled |
                       vk::ImageUsageFlagBits::eTransferDst;
        img_ci.initialLayout = vk::ImageLayout::eUndefined;
        fr.hiz_image = vk::raii::Image(dev, img_ci);

        VulkanBackend::Vulkan::SetVulkanObjectName(dev, fr.hiz_image, "hiz-image");

        const vk::MemoryRequirements mem_req = fr.hiz_image.getMemoryRequirements();
        vk::MemoryAllocateInfo alloc_ci{};
        alloc_ci.allocationSize = mem_req.size;
        alloc_ci.memoryTypeIndex = VulkanBackend::Vulkan::MemoryUtils::FindMemoryType(
            be.GetPhysicalDevice(), mem_req.memoryTypeBits,
            vk::MemoryPropertyFlagBits::eDeviceLocal);
        fr.hiz_memory = vk::raii::DeviceMemory(dev, alloc_ci);
        VulkanBackend::Vulkan::SetVulkanObjectName(dev, fr.hiz_memory, "hiz-memory");
        fr.hiz_image.bindMemory(*fr.hiz_memory, 0);

        fr.hiz_mip_views.clear();
        fr.hiz_mip_views.reserve(mip_levels);
        for (std::uint32_t mip = 0; mip < mip_levels; ++mip) {
            vk::ImageViewCreateInfo view_ci{};
            view_ci.image = *fr.hiz_image;
            view_ci.viewType = vk::ImageViewType::e2D;
            view_ci.format = hiz_format;
            view_ci.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
            view_ci.subresourceRange.baseMipLevel = mip;
            view_ci.subresourceRange.levelCount = 1;
            view_ci.subresourceRange.baseArrayLayer = 0;
            view_ci.subresourceRange.layerCount = 1;
            fr.hiz_mip_views.emplace_back(dev, view_ci);
            VulkanBackend::Vulkan::SetVulkanObjectName(dev, fr.hiz_mip_views.back(), "hiz-mip-view-" + std::to_string(mip));
        }

        vk::ImageViewCreateInfo full_view_ci{};
        full_view_ci.image = *fr.hiz_image;
        full_view_ci.viewType = vk::ImageViewType::e2D;
        full_view_ci.format = hiz_format;
        full_view_ci.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
        full_view_ci.subresourceRange.baseMipLevel = 0;
        full_view_ci.subresourceRange.levelCount = mip_levels;
        full_view_ci.subresourceRange.baseArrayLayer = 0;
        full_view_ci.subresourceRange.layerCount = 1;
        fr.hiz_full_view = vk::raii::ImageView(dev, full_view_ci);
        VulkanBackend::Vulkan::SetVulkanObjectName(dev, fr.hiz_full_view, "hiz-full-view");

        // Storage images, sampler and the depth placeholder; binding numbers come from the hiz set's table rows.
        WriteHiZBindings(fr);
    }

#ifndef NDEBUG
    // Invariant: DispatchHiZGen must produce every level the cull shaders can
    // sample. The generator's coverage and the shaders' MaxHizMip() clamp are
    // independent derivations of the same range, so assert they agree; a
    // regression here leaves a sampled mip unwritten, which reads as undefined
    // memory and false-culls large screen footprints on some launches.
    {
        [[maybe_unused]] std::uint32_t cull_max_mip = 0; // == floor(log2(max(hiz_w, hiz_h)))
        for (std::uint32_t m = std::max(hiz_w, hiz_h); m > 1; m >>= 1) ++cull_max_mip;
        assert(LastHiZLevelWritten(hiz_mip_count_) == hiz_mip_count_ - 1 &&
               "Hi-Z generator does not cover every level; cull shaders may sample an unwritten mip");
        assert(cull_max_mip <= LastHiZLevelWritten(hiz_mip_count_) &&
               "cull shaders can sample a Hi-Z mip the generator never writes");
    }
#endif

    return true;
}

void SceneRenderer::EnsureRenderExtent(std::uint32_t width, std::uint32_t height) {
    if (!backend_ || width == 0 || height == 0) return;
    if (width == depth_width_ && height == depth_height_) return;

    // Swapchain recreation already idled the device, but match the
    // EnsureSceneCapacity/Reinitialize pattern so this is safe however it is
    // reached. Hi-Z is derived per-frame (non-temporal), so the old pyramid is
    // discarded rather than rescaled; the next frame's hiz-gen refills it.
    backend_->GetDevice().waitIdle();

    DestroyHiZResources();
    depth_width_ = width;
    depth_height_ = height;
    if (!CreateHiZResources()) {
        LOGIFACE_LOG(error, "SceneRenderer::EnsureRenderExtent: failed to create Hi-Z resources");
        return;
    }
    hiz_initialized_ = false;

    LOGIFACE_LOG(info, "SceneRenderer: render extent changed to " +
        std::to_string(width) + "x" + std::to_string(height) +
        " (Hi-Z mips=" + std::to_string(hiz_mip_count_) + ")");
}

void SceneRenderer::EnsureSceneCapacity(const SceneCapacity& required) {
    if (!backend_) return;
    if (required.index_count <= scene_capacity_.index_count &&
        required.vertex_span <= scene_capacity_.vertex_span &&
        required.submesh_count <= scene_capacity_.submesh_count &&
        required.deform_vertex_span <= scene_capacity_.deform_vertex_span &&
        required.palette_matrix_count <= scene_capacity_.palette_matrix_count &&
        required.modifier_param_bytes <= scene_capacity_.modifier_param_bytes) {
        return;
    }
    backend_->GetDevice().waitIdle();
    scene_capacity_.index_count =
        std::max(required.index_count, scene_capacity_.index_count * 2);
    scene_capacity_.vertex_span =
        std::max(required.vertex_span, scene_capacity_.vertex_span * 2);
    scene_capacity_.submesh_count =
        std::max(required.submesh_count, scene_capacity_.submesh_count * 2);
    scene_capacity_.deform_vertex_span =
        std::max(required.deform_vertex_span, scene_capacity_.deform_vertex_span * 2);
    scene_capacity_.palette_matrix_count =
        std::max(required.palette_matrix_count, scene_capacity_.palette_matrix_count * 2);
    scene_capacity_.modifier_param_bytes =
        std::max(required.modifier_param_bytes, scene_capacity_.modifier_param_bytes * 2);
    LOGIFACE_LOG(info, "SceneRenderer: capacity grew to index_count=" +
        std::to_string(scene_capacity_.index_count) + " vertex_span=" +
        std::to_string(scene_capacity_.vertex_span) + " submeshes=" +
        std::to_string(scene_capacity_.submesh_count) + " deform_span=" +
        std::to_string(scene_capacity_.deform_vertex_span) + " palette_matrices=" +
        std::to_string(scene_capacity_.palette_matrix_count) + " modifier_bytes=" +
        std::to_string(scene_capacity_.modifier_param_bytes));
    DestroyFrameBuffers();
    CreateFrameBuffers();
    // Buffer descriptors are rewritten each frame in PrepareCompute.
}

void SceneRenderer::Reinitialize(DrawMode mode) {
    if (mode == draw_mode_) return;
    if (!IsDrawModeSupported(mode)) {
        LOGIFACE_LOG(warn, "SceneRenderer::Reinitialize: requested draw mode unsupported; ignoring");
        return;
    }
    backend_->GetDevice().waitIdle();
    DestroyFrameBuffers();
    draw_mode_ = mode;
    CreateFrameBuffers();
    if (!RebuildCompactionPipelines()) {
        LOGIFACE_LOG(error, "SceneRenderer::Reinitialize: failed to rebuild compaction pipelines");
    }
    LOGIFACE_LOG(info, std::string("SceneRenderer: draw mode set to ") +
        (mode == DrawMode::MID ? "MID" : "CID"));
}

namespace {
    template<typename Desc>
    std::optional<ShaderSystem::PipelineProduct> RebuildForDesc(
        ShaderSystem::PipelineFactory& factory, const Desc& desc,
        ShaderSystem::ShaderManager& shaders) {
        if constexpr (std::same_as<Desc, ShaderSystem::ComputePipelineDesc>) {
            auto result = factory.CreateCompute(desc, shaders);
            return result
                ? std::optional<ShaderSystem::PipelineProduct>(std::move(*result))
                : std::nullopt;
        } else {
            auto result = factory.CreateGraphics(desc, shaders);
            return result
                ? std::optional<ShaderSystem::PipelineProduct>(std::move(*result))
                : std::nullopt;
        }
    }
} // anonymous namespace

void SceneRenderer::PollShaders(std::uint32_t frame_counter) {
    const auto poll = [this, frame_counter](
            ShaderSystem::PipelineSlot& slot, ShaderSystem::ShaderId id,
            auto& desc) {
        slot.RetireFrame(frame_counter);
        if (!desc) return;
        slot.PollAndRebuild(*shader_mgr_, id,
            [this, &desc](ShaderSystem::ShaderManager& s)
                -> std::optional<ShaderSystem::PipelineProduct> {
                return RebuildForDesc(*pipeline_factory_, *desc, s);
            }, frame_counter);
    };
    const auto poll_graphics = [this, frame_counter](
            ShaderSystem::PipelineSlot& slot,
            ShaderSystem::ShaderId vert_id, ShaderSystem::ShaderId frag_id,
            auto& desc) {
        slot.RetireFrame(frame_counter);
        if (!desc) return;
        slot.PollAndRebuild(*shader_mgr_, vert_id, frag_id,
            [this, &desc](ShaderSystem::ShaderManager& s)
                -> std::optional<ShaderSystem::PipelineProduct> {
                return RebuildForDesc(*pipeline_factory_, *desc, s);
            }, frame_counter);
    };
    poll(expand_slot_, shader_ids_.expand_comp, expand_desc_);
    poll(occlusion_slot_, shader_ids_.occlusion_cull_comp, occlusion_desc_);
    poll(occluder_select_slot_, shader_ids_.occluder_select_comp, occluder_select_desc_);
    poll(occluder_select_mid_slot_, shader_ids_.occluder_select_comp_mid,
         occluder_select_mid_desc_);
    poll(pre_cull_slot_, shader_ids_.pre_cull_comp, pre_cull_desc_);
    poll(pre_cull_mid_slot_, shader_ids_.pre_cull_comp_mid, pre_cull_mid_desc_);
    poll(hiz_slot_, shader_ids_.hiz_gen_comp, hiz_desc_);
    poll(collect_count_slot_, shader_ids_.collect_count_compact_comp, collect_count_desc_);
    poll(collect_count_mid_slot_, shader_ids_.collect_count_compact_comp_mid,
         collect_count_mid_desc_);
    poll(collect_write_slot_, shader_ids_.collect_write_comp, collect_write_desc_);
    poll_graphics(depth_slot_, shader_ids_.depth_indir_vert, shader_ids_.depth_prepass_frag, depth_desc_);
}

void SceneRenderer::Shutdown() {
    for (auto& fr : frames_) {
        for (const SceneGpuLayout::BlockArraySpec& spec : SceneGpuLayout::kBlockArrays) {
            fr.BlockArrayFor(spec.slot).Shutdown();
        }
        for (const SceneGpuLayout::BufferSpec& spec : SceneGpuLayout::kFrameBuffers) {
            fr.BufferFor(spec.slot) = GpuResources::GpuBuffer{};
        }
        for (const SceneGpuLayout::SetSpec& set : SceneGpuLayout::kSceneSets) {
            if (set.fixed_sets != 0) continue;   // the lighting set is not ringed
            fr.SetFor(set.id) = GpuResources::GpuDescriptorSet{};
        }
    }
    frame_modifier_dispatches_.clear();
    has_active_deform_ = false;
    // Pipelines reference the set layouts, so drop them before the sets.
    modifier_pipelines_.clear();
    modifier_registry_version_ = 0;
    deform_pipeline_layout_.reset();
    modifier_mgr_ = nullptr;
    expand_pipeline_layout_.reset();
    occlusion_pipeline_layout_.reset();
    occluder_select_pipeline_layout_.reset();
    pre_cull_pipeline_layout_.reset();
    collect_pipeline_layout_.reset();
    collect_write_pipeline_layout_.reset();
    depth_pipeline_layout_.reset();
    hiz_pipeline_layout_.reset();
    scene_lights.Shutdown();
    scene_header = GpuResources::GpuBuffer{};
    technique_flags = GpuResources::GpuBuffer{};
    technique_flags_cache_.clear();
    for (auto& owned : owned_sets_) owned = GpuResources::GpuDescriptorSet{};
    // Drops every set layout and pool. The sets above already released theirs.
    sets_ = {};
    backend_ = nullptr;
}

namespace {
// A layout handle for the public accessors, or null when the renderer is not initialized.
// The accessors hand out a pointer that the caller dereferences.
vk::DescriptorSetLayout* LayoutPointer(
    const std::unique_ptr<vk::raii::DescriptorSetLayout>& layout) {
    return layout ? const_cast<vk::DescriptorSetLayout*>(&**layout) : nullptr;
}
} // anonymous namespace

vk::DescriptorSetLayout* SceneRenderer::GetSubmeshVertexEntriesLayout() const {
    return LayoutPointer(sets_[static_cast<std::size_t>(SceneGpuLayout::SetId::submesh_vertex)].layout);
}

vk::DescriptorSetLayout* SceneRenderer::GetVertexBuffersLayout() const {
    return LayoutPointer(sets_[static_cast<std::size_t>(SceneGpuLayout::SetId::vertex_buffers)].layout);
}

vk::DescriptorSetLayout* SceneRenderer::GetIndirectionLayout() const {
    return LayoutPointer(sets_[static_cast<std::size_t>(SceneGpuLayout::SetId::indirection)].layout);
}

void SceneRenderer::UpdateVertexBufferArrayElement(std::uint32_t frame_index,
                                                     std::uint32_t buffer_index,
                                                     vk::Buffer buffer,
                                                     std::uint64_t size) {
    const auto& fr = frames_[frame_index % frames_in_flight_];
    if (!buffer) {
        LOGIFACE_LOG(warn, "UpdateVertexBufferArrayElement: null buffer for slot " +
                     std::to_string(buffer_index));
        return;
    }
    const vk::DescriptorBufferInfo bii(buffer, 0, size);
    vk::WriteDescriptorSet w{};
    w.dstSet = fr.vertex_buffers_set.GetHandle();
    w.dstBinding = kVertexTableBinding;
    w.dstArrayElement = buffer_index;
    w.descriptorCount = 1;
    w.descriptorType = vk::DescriptorType::eStorageBuffer;
    w.pBufferInfo = &bii;
    backend_->GetDevice().updateDescriptorSets(w, nullptr);
}

void SceneRenderer::UpdateIndexBufferArrayElement(std::uint32_t frame_index,
                                                    std::uint32_t buffer_index,
                                                    vk::Buffer buffer,
                                                    std::uint64_t size) {
    const auto& fr = frames_[frame_index % frames_in_flight_];
    if (!buffer) {
        LOGIFACE_LOG(warn, "UpdateIndexBufferArrayElement: null buffer for slot " +
                     std::to_string(buffer_index));
        return;
    }
    const vk::DescriptorBufferInfo bii(buffer, 0, size);
    vk::WriteDescriptorSet w{};
    w.dstSet = fr.index_buffers_set.GetHandle();
    w.dstBinding = kIndexTableBinding;
    w.dstArrayElement = buffer_index;
    w.descriptorCount = 1;
    w.descriptorType = vk::DescriptorType::eStorageBuffer;
    w.pBufferInfo = &bii;
    backend_->GetDevice().updateDescriptorSets(w, nullptr);
}

void SceneRenderer::UpdateUvBufferArrayElement(std::uint32_t frame_index,
                                                std::uint32_t buffer_index,
                                                vk::Buffer buffer,
                                                std::uint64_t size) {
    const auto& fr = frames_[frame_index % frames_in_flight_];
    if (!buffer) {
        LOGIFACE_LOG(warn, "UpdateUvBufferArrayElement: null buffer for slot " +
                     std::to_string(buffer_index));
        return;
    }
    const vk::DescriptorBufferInfo bii(buffer, 0, size);
    vk::WriteDescriptorSet w{};
    w.dstSet = fr.vertex_buffers_set.GetHandle();
    w.dstBinding = kUvTableBinding;
    w.dstArrayElement = buffer_index;
    w.descriptorCount = 1;
    w.descriptorType = vk::DescriptorType::eStorageBuffer;
    w.pBufferInfo = &bii;
    backend_->GetDevice().updateDescriptorSets(w, nullptr);
}

void SceneRenderer::UpdateAllFrameUvBufferArrayElements(std::uint32_t buffer_index,
                                                         vk::Buffer buffer,
                                                         std::uint64_t size) {
    for (std::uint32_t fi = 0; fi < frames_in_flight_; ++fi) {
        UpdateUvBufferArrayElement(fi, buffer_index, buffer, size);
    }
}

void SceneRenderer::UpdateAllFrameVertexBufferArrayElements(std::uint32_t buffer_index,
                                                              vk::Buffer buffer,
                                                              std::uint64_t size) {
    for (std::uint32_t fi = 0; fi < frames_in_flight_; ++fi) {
        UpdateVertexBufferArrayElement(fi, buffer_index, buffer, size);
    }
}

void SceneRenderer::UpdateAllFrameIndexBufferArrayElements(std::uint32_t buffer_index,
                                                             vk::Buffer buffer,
                                                             std::uint64_t size) {
    for (std::uint32_t fi = 0; fi < frames_in_flight_; ++fi) {
        UpdateIndexBufferArrayElement(fi, buffer_index, buffer, size);
    }
}

namespace {

// Writes one element of a fixed-size skin-influence descriptor array. The table
// lives in the deform set, which is allocated once and outlives capacity growth,
// so a descriptor written here stays valid for the life of the set.
void WriteSkinTableElement(const vk::raii::Device& dev,
                           const vk::DescriptorSet set,
                           const std::uint32_t binding,
                           const std::uint32_t buffer_index,
                           const vk::Buffer buffer,
                           const std::uint64_t size,
                           const char* const what) {
    if (!set || !buffer) {
        LOGIFACE_LOG(warn, std::string(what) + ": null set or buffer for skin slot " +
                     std::to_string(buffer_index));
        return;
    }
    // The table is a fixed MAX_SKIN_BUFFERS-element descriptor array. Writing past it
    // is invalid descriptor usage, so the bound is enforced here as well as at the
    // allocation site: this is the one choke point every skin-table write goes
    // through.
    if (buffer_index >= SceneRenderer::MAX_SKIN_BUFFERS) {
        static bool reported = false;
        if (!reported) {
            reported = true;
            LOGIFACE_LOG(error, std::string(what) + ": skin buffer slot " +
                         std::to_string(buffer_index) + " is past the " +
                         std::to_string(SceneRenderer::MAX_SKIN_BUFFERS) +
                         "-slot deform table; the descriptor is not written");
        }
        return;
    }
    const vk::DescriptorBufferInfo bii(buffer, 0, size);
    vk::WriteDescriptorSet w{};
    w.dstSet = set;
    w.dstBinding = binding;
    w.dstArrayElement = buffer_index;
    w.descriptorCount = 1;
    w.descriptorType = vk::DescriptorType::eStorageBuffer;
    w.pBufferInfo = &bii;
    dev.updateDescriptorSets(w, nullptr);
}

} // anonymous namespace

void SceneRenderer::UpdateSkinBufferArrayElement(std::uint32_t frame_index,
                                                 std::uint32_t buffer_index,
                                                 vk::Buffer buffer,
                                                 std::uint64_t size) {
    if (!backend_ || frames_.empty()) return;
    WriteSkinTableElement(backend_->GetDevice(),
                          frames_[frame_index % frames_in_flight_].deform_set.GetHandle(),
                          kSkinTableBinding, buffer_index, buffer, size,
                          "UpdateSkinBufferArrayElement");
}

void SceneRenderer::UpdateSkinExtraBufferArrayElement(std::uint32_t frame_index,
                                                      std::uint32_t buffer_index,
                                                      vk::Buffer buffer,
                                                      std::uint64_t size) {
    if (!backend_ || frames_.empty()) return;
    WriteSkinTableElement(backend_->GetDevice(),
                          frames_[frame_index % frames_in_flight_].deform_set.GetHandle(),
                          kSkinExtraTableBinding, buffer_index, buffer, size,
                          "UpdateSkinExtraBufferArrayElement");
}

void SceneRenderer::UpdateAllFrameSkinBufferArrayElements(std::uint32_t buffer_index,
                                                           vk::Buffer buffer,
                                                           std::uint64_t size) {
    for (std::uint32_t fi = 0; fi < frames_in_flight_; ++fi) {
        UpdateSkinBufferArrayElement(fi, buffer_index, buffer, size);
    }
}

void SceneRenderer::UpdateAllFrameSkinExtraBufferArrayElements(std::uint32_t buffer_index,
                                                                vk::Buffer buffer,
                                                                std::uint64_t size) {
    for (std::uint32_t fi = 0; fi < frames_in_flight_; ++fi) {
        UpdateSkinExtraBufferArrayElement(fi, buffer_index, buffer, size);
    }
}

GpuResources::BlockArray& SceneRenderer::GetFrameSkinEntries(std::uint32_t frame_index) {
    return frames_[frame_index % frames_in_flight_].skin_entries;
}

std::span<std::array<float, 16>> SceneRenderer::MapFrameBonePalettes(std::uint32_t frame_index) {
    if (!backend_ || frames_.empty()) return {};
    auto& buffer = frames_[frame_index % frames_in_flight_].bone_palettes;
    if (!buffer.IsValid()) return {};
    const std::uint64_t bytes = buffer.GetSize();
    auto* mapped = static_cast<std::array<float, 16>*>(buffer.Map(0, bytes));
    if (mapped == nullptr) return {};
    return {mapped, static_cast<std::size_t>(bytes / (16u * sizeof(float)))};
}

void SceneRenderer::UnmapFrameBonePalettes(std::uint32_t frame_index) {
    if (!backend_ || frames_.empty()) return;
    auto& buffer = frames_[frame_index % frames_in_flight_].bone_palettes;
    if (buffer.IsValid()) buffer.Unmap();
}

std::span<std::byte> SceneRenderer::MapFrameModifierParams(std::uint32_t frame_index) {
    if (!backend_ || frames_.empty()) return {};
    auto& buffer = frames_[frame_index % frames_in_flight_].modifier_params;
    if (!buffer.IsValid()) return {};
    const std::uint64_t bytes = buffer.GetSize();
    auto* mapped = static_cast<std::byte*>(buffer.Map(0, bytes));
    if (mapped == nullptr) return {};
    return {mapped, static_cast<std::size_t>(bytes)};
}

void SceneRenderer::UnmapFrameModifierParams(std::uint32_t frame_index) {
    if (!backend_ || frames_.empty()) return;
    auto& buffer = frames_[frame_index % frames_in_flight_].modifier_params;
    if (buffer.IsValid()) buffer.Unmap();
}

void SceneRenderer::SetModifierDispatches(std::uint32_t frame_index,
                                          std::span<const ModifierDispatch> dispatches) {
    if (frame_modifier_dispatches_.empty()) return;
    auto& dst = frame_modifier_dispatches_[frame_index % frames_in_flight_];
    dst.assign(dispatches.begin(), dispatches.end());
}

std::span<const ModifierDispatch> SceneRenderer::GetModifierDispatches(
    std::uint32_t frame_index) const {
    if (frame_modifier_dispatches_.empty()) return {};
    return frame_modifier_dispatches_[frame_index % frames_in_flight_];
}

bool SceneRenderer::IsModifierReady(const Modifiers::ModifierId id) const {
    if (!id.IsValid()) return false;
    const auto it = modifier_pipelines_.find(ModifierKey(id));
    return it != modifier_pipelines_.end() && it->second.ready && !it->second.dead &&
           it->second.slot.Get() != nullptr;
}

void SceneRenderer::SyncModifierPipelines(const std::uint32_t frame_counter) {
    if (!backend_ || !modifier_mgr_ || !shader_mgr_ || !pipeline_factory_ ||
        !deform_pipeline_layout_) {
        // Without the deform layout no modifier can be built. Gather then sees
        // IsModifierReady == false and every skinned submesh renders its bind
        // pose.
        return;
    }

    // Retire unconditionally: every slot's retire ring has to advance once per
    // frame, whether or not anything changed.
    for (auto& [key, pipeline] : modifier_pipelines_) {
        pipeline.slot.RetireFrame(frame_counter);
    }
    // A retired Vulkan pipeline may still be referenced by a submitted command
    // buffer until frames_in_flight_ frames have passed.
    std::erase_if(modifier_pipelines_, [frame_counter](const auto& item) {
        return item.second.dead && frame_counter >= item.second.dead_at;
    });

    const std::uint32_t fif = std::max(frames_in_flight_, 1u);
    const std::uint64_t version = modifier_mgr_->RegistryVersion();
    if (version != modifier_registry_version_) {
        modifier_registry_version_ = version;
        // A retired registry entry keeps its pipeline alive for the full retire
        // window; gather stops emitting it the moment IsModifierReady sees the
        // dead flag.
        for (auto& [key, pipeline] : modifier_pipelines_) {
            if (!pipeline.dead && !modifier_mgr_->IsAlive(pipeline.id)) {
                pipeline.dead = true;
                pipeline.ready = false;
                pipeline.dead_at = frame_counter + fif;
            }
        }
        // New entries get their pipeline object here; the build below happens
        // this same frame so gather already sees it as ready.
        for (const auto& entry : modifier_mgr_->Entries()) {
            if (!entry.alive) continue;
            const std::uint64_t key = ModifierKey(entry.id);
            if (modifier_pipelines_.contains(key)) continue;
            const Modifiers::ModifierDesc* desc = modifier_mgr_->Get(entry.id);
            if (desc == nullptr || desc->compute_shader == ShaderSystem::kInvalidShaderId) {
                continue;
            }
            ModifierPipeline pipeline{};
            pipeline.id = entry.id;
            pipeline.slot.SetFramesInFlight(fif);
            ShaderSystem::ComputePipelineDesc pipeline_desc{};
            pipeline_desc.shader = desc->compute_shader;
            pipeline_desc.layout = **deform_pipeline_layout_;
            pipeline_desc.entry_point = desc->entry_point;
            pipeline.desc = std::move(pipeline_desc);
            modifier_pipelines_.emplace(key, std::move(pipeline));
        }
    }

    // Build or rebuild each live pipeline whose shader version changed. Polling
    // every frame (a version lookup per pipeline, not per submesh) is what makes
    // a deform.slang edit hot-reload; the registry version above only gates
    // creation and retirement.
    for (auto& [key, pipeline] : modifier_pipelines_) {
        if (pipeline.dead || !pipeline.desc) continue;
        const std::uint64_t shader_version =
            shader_mgr_->GetVersion(pipeline.desc->shader);
        const bool version_changed = pipeline.shader_version != shader_version;
        // Steady state: a live pipeline whose shader has not changed is left
        // alone. A pipeline that is not ready (a failed build) keeps retrying so
        // a transient driver failure heals without a shader edit.
        if (pipeline.ready && !version_changed) continue;
        if (version_changed) {
            // A new shader version gets a fresh chance and a fresh error line.
            pipeline.shader_version = shader_version;
            pipeline.failure_logged = false;
        }
        auto result = pipeline_factory_->CreateCompute(*pipeline.desc, *shader_mgr_);
        if (!result.has_value()) {
            // Keep retrying: a transient driver failure heals on its own, and a
            // permanently broken modifier stays invisible to gather (bind pose)
            // rather than failing the frame. The failure line is emitted once per
            // shader version, not once per frame.
            pipeline.ready = false;
            if (!pipeline.failure_logged) {
                pipeline.failure_logged = true;
                const Modifiers::ModifierDesc* desc = modifier_mgr_->Get(pipeline.id);
                LOGIFACE_LOG(error, "SyncModifierPipelines: modifier '" +
                                        (desc != nullptr ? desc->name : std::string("<stale>")) +
                                        "' pipeline creation failed (will retry): " +
                                        result.error().message);
            }
            continue;
        }
        pipeline.slot.Swap(std::move(*result), frame_counter);
        pipeline.ready = true;
        pipeline.failure_logged = false;
        const Modifiers::ModifierDesc* desc = modifier_mgr_->Get(pipeline.id);
        VulkanBackend::Vulkan::SetVulkanObjectName(
            backend_->GetDevice(), pipeline.slot.Get(),
            "modifier-" + (desc != nullptr ? desc->name : std::string("stale")) + "-pipeline");
    }
}

Modifiers::ModifierId SceneRenderer::SkinningModifier() const {
    return modifier_mgr_ != nullptr ? modifier_mgr_->SkinningId() : Modifiers::kInvalidModifierId;
}

vk::Buffer SceneRenderer::GetFrameDeformVertices(const std::uint32_t frame_index) const {
    if (frames_.empty()) return {};
    const auto& buffer = frames_[frame_index % frames_in_flight_].deform_vertices;
    return buffer.IsValid() ? *buffer.GetBuffer() : vk::Buffer{};
}

VulkanEngine::GpuResources::BlockArray& SceneRenderer::GetFrameVertexEntries(
    const std::uint32_t frame_index) {
    return frames_[frame_index % frames_in_flight_].submesh_vertex_entries;
}

VulkanEngine::GpuResources::BlockArray& SceneRenderer::GetFrameCullEntries(
    const std::uint32_t frame_index) {
    return frames_[frame_index % frames_in_flight_].cull_entries;
}

void SceneRenderer::UpdateHizDepthBinding(std::uint32_t frame_index, vk::ImageView depth_view) {
    auto& fr = frames_[frame_index % frames_in_flight_];
    if (!backend_) return;
    auto& dev = backend_->GetDevice();
    const std::uint32_t binding = SceneGpuLayout::BindingOfKind(
        SceneGpuLayout::SetId::hiz, vk::DescriptorType::eSampledImage);
    if (binding == SceneGpuLayout::kNoBinding) return;
    const vk::DescriptorImageInfo depth_info(
        nullptr, depth_view, vk::ImageLayout::eShaderReadOnlyOptimal);
    vk::WriteDescriptorSet w{};
    w.dstSet = fr.hiz_set.GetHandle();
    w.dstBinding = binding;
    w.descriptorCount = 1;
    w.descriptorType = vk::DescriptorType::eSampledImage;
    w.pImageInfo = &depth_info;
    dev.updateDescriptorSets(w, nullptr);
}

SceneRenderer::FrameBlockArrays SceneRenderer::GetFrameBlockArrays(std::uint32_t frame_index) {
    auto& fr = frames_[frame_index % frames_in_flight_];
    return {
        &fr.dynamic_entries,
        &fr.static_entries,
        &fr.bounding_spheres,
        &fr.obb_entries
    };
}

void SceneRenderer::UploadLighting(const SceneHeader& header,
                                    std::span<const Light> lights,
                                    GpuResources::StagingPool& staging) {
    if (!backend_) return;

    // Grow BlockArray to fit all lights. The descriptor binding is a fixed
    // MAX_LIGHT_BLOCKS array, so the upload is clamped to what actually fits.
    constexpr std::size_t kLightCapacity =
        static_cast<std::size_t>(MAX_LIGHT_BLOCKS) * LIGHTS_PER_BLOCK;
    if (lights.size() > kLightCapacity) {
        static bool reported_overflow = false;
        if (!reported_overflow) {
            reported_overflow = true;
            LOGIFACE_LOG(error, "SceneRenderer::UploadLighting: " +
                std::to_string(lights.size()) + " lights exceed the " +
                std::to_string(kLightCapacity) +
                "-light descriptor capacity; extra lights are dropped");
        }
    }
    const std::size_t light_count = std::min<std::size_t>(lights.size(), kLightCapacity);
    if (!scene_lights.EnsureCapacity(static_cast<std::uint32_t>(light_count))) {
        LOGIFACE_LOG(error, "SceneRenderer::UploadLighting: light BlockArray could not grow to hold " +
            std::to_string(light_count) + " lights");
    }
    const std::size_t uploadable = std::min<std::size_t>(light_count,
        static_cast<std::size_t>(scene_lights.BlockCount()) * scene_lights.EntriesPerBlock());

    // Stage the header buffer
    {
        auto header_slice = staging.Allocate(sizeof(SceneHeader));
        if (header_slice.has_value()) {
            std::memcpy(header_slice->mapped_ptr, &header, sizeof(SceneHeader));
            staging.RecordBufferCopy(*header_slice,
                                     static_cast<vk::Buffer>(*scene_header.GetBuffer()), 0);
        }
    }

    // Stage each light via BlockArray::UploadEntry (uses staging internally)
    for (std::size_t i = 0; i < uploadable; ++i) {
        scene_lights.UploadEntry(static_cast<std::uint32_t>(i), &lights[i],
                                         sizeof(Light), staging);
    }

    // Write BlockArray buffers to descriptor set binding 1
    const auto& dev = backend_->GetDevice();
    for (std::uint32_t bi = 0; bi < scene_lights.BlockCount(); ++bi) {
        const vk::DescriptorBufferInfo buf_info(scene_lights.GetBlockArray(bi),
                                                 0, scene_lights.BlockSize());
        vk::WriteDescriptorSet w{};
        w.dstSet = GetSceneUniformSet();
        w.dstBinding = kLightTableBinding;
        w.dstArrayElement = bi;
        w.descriptorCount = 1;
        w.descriptorType = vk::DescriptorType::eStorageBuffer;
        w.pBufferInfo = &buf_info;
        dev.updateDescriptorSets(w, nullptr);
    }

    staging.FlushImmediate();
}

} // namespace VulkanEngine::SceneRenderer
