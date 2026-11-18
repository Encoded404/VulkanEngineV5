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

// Short names for the slot enums. The tables own the numbering; these only keep the call sites readable.
using SetIdx = SceneGpuLayout::SetId;
using BufIdx = SceneGpuLayout::BufferSlot;
using BlkIdx = SceneGpuLayout::BlockSlot;

// SceneLimits is Vulkan-free, so its record sizes are plain integers; they are pinned to the types they describe here, where both are visible.
static_assert(sizeof(vk::DrawIndexedIndirectCommand) == SceneLimits::kIndirectCommandBytes,
              "SceneLimits::kIndirectCommandBytes must equal vk::DrawIndexedIndirectCommand");
static_assert(sizeof(SkinEntry) == SceneLimits::kSkinEntryBytes,
              "SceneLimits::kSkinEntryBytes must equal SkinEntry");
static_assert(sizeof(ModifierPC) ==
                  SceneGpuLayout::LayoutOf(SceneGpuLayout::PipelineLayoutId::deform)
                      .push_constant_bytes,
              "the deform layout's push-constant range must equal ModifierPC");

namespace {

// The set rows carry vk:: flag words already, so the only translation left is the block-array memory kind, which has to produce a BlockArray::MemoryMode as well as a flag word.

constexpr vk::MemoryPropertyFlags ToVkMemory(const SceneGpuLayout::BufMemory memory) {
    return memory == SceneGpuLayout::BufMemory::host_visible
        ? vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent
        : vk::MemoryPropertyFlags(vk::MemoryPropertyFlagBits::eDeviceLocal);
}

constexpr GpuResources::MemoryMode ToMemoryMode(const SceneGpuLayout::BufMemory memory) {
    return memory == SceneGpuLayout::BufMemory::device_local
        ? GpuResources::MemoryMode::DeviceLocal
        : GpuResources::MemoryMode::HostVisible;
}

// Layout creation from one set row.
// The create info borrows `bindings` and `binding_flags`, so the layout is created here, while both are alive; there is no half-built plan to keep in its final place.

vk::raii::DescriptorSetLayout CreateSetLayout(const vk::raii::Device& dev,
                                              const SceneGpuLayout::SetSpec& set,
                                              const std::uint32_t max_blocks) {
    std::vector<vk::DescriptorSetLayoutBinding> bindings;
    std::vector<vk::DescriptorBindingFlags> binding_flags;
    bindings.reserve(set.bindings.size());
    binding_flags.reserve(set.bindings.size());
    for (const SceneGpuLayout::BindingSpec& row : set.bindings) {
        bindings.push_back(vk::DescriptorSetLayoutBinding{
            row.binding, row.kind, SceneGpuLayout::DescriptorCount(row, max_blocks), row.stages});
        binding_flags.push_back(row.flags);
    }

    // pNext carries one flag word per binding; an all-zero word means no special behaviour, so attaching it is safe for every set.
    const vk::DescriptorSetLayoutBindingFlagsCreateInfo flag_info(
        static_cast<std::uint32_t>(binding_flags.size()), binding_flags.data());
    const vk::DescriptorSetLayoutCreateInfo info{
        // Required when any binding uses eUpdateAfterBind, and required on the pool the set is allocated from.
        // Deriving it from the rows keeps the layout flag, the pool flag and the binding flags from disagreeing.
        SceneGpuLayout::UpdatesAfterBind(set.id)
            ? vk::DescriptorSetLayoutCreateFlags(
                  vk::DescriptorSetLayoutCreateFlagBits::eUpdateAfterBindPool)
            : vk::DescriptorSetLayoutCreateFlags{},
        static_cast<std::uint32_t>(bindings.size()),
        bindings.data(),
        bindings.empty() ? nullptr : static_cast<const void*>(&flag_info)};
    return {dev, info};
}

GpuResources::DescriptorPoolConfig ToPoolConfig(const SceneGpuLayout::SetSpec& set,
                                                const SceneGpuLayout::PoolCounts& counts) {
    return {
        .max_sets = counts.max_sets,
        .max_combined_image_samplers = counts.combined_image_samplers,
        .max_uniform_buffers = 0,
        .max_storage_buffers = counts.storage_buffers,
        .max_storage_images = counts.storage_images,
        .max_sampled_images = counts.sampled_images,
        .max_samplers = counts.samplers,
        // A layout created with eUpdateAfterBindPool requires a pool with the same flag, and both must agree with the binding flags.
        .update_after_bind = SceneGpuLayout::UpdatesAfterBind(set.id),
    };
}

} // anonymous namespace

SceneRenderer::~SceneRenderer() {
    Shutdown();
}

vk::DescriptorSetLayout SceneRenderer::GetSetLayout(const SceneGpuLayout::SetId id) const {
    const SetHandles& slot = sets_[id];
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

void SceneRenderer::ClearBuffer(GpuResources::GpuBuffer& buffer) {
    // A host-visible buffer is mapped directly; the rows that ask for a clear are all host-visible, so a failed map means the buffer was never created for this mode.
    if (!buffer.IsValid()) return;
    void* mapped = buffer.Map(0, buffer.GetSize());
    if (mapped == nullptr) return;
    std::memset(mapped, 0, static_cast<std::size_t>(buffer.GetSize()));
    buffer.Unmap();
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
        slot.layout = std::make_unique<vk::raii::DescriptorSetLayout>(
            CreateSetLayout(dev, set, max_blocks_));
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
        const SceneGpuLayout::BindingSpec* variable = SceneGpuLayout::VariableRow(set.id);
        if (variable == nullptr) {
            return slot.pool->Allocate(**slot.layout);
        }
        return slot.pool->Allocate(**slot.layout,
                                   SceneGpuLayout::DescriptorCount(*variable, max_blocks_));
    };

    // A fixed row owns one set, allocated here from the pool its `fixed_sets` sized.
    for (const SceneGpuLayout::SetSpec& set : SceneGpuLayout::kSceneSets) {
        if (set.fixed_sets == 0) continue;
        owned_sets_[set.id] = allocate(set);
        owned_sets_[set.id].SetDebugName(dev, std::string(set.name) + "-set");
    }

    // Every other row gets one set per frame in flight.
    for (auto& fr : frames_) {
        for (const SceneGpuLayout::SetSpec& set : SceneGpuLayout::kSceneSets) {
            if (set.fixed_sets != 0) continue;
            fr[set.id] = allocate(set);
            fr[set.id].SetDebugName(dev, std::string(set.name) + "-set");
        }
    }

    // Publish the vertex-table elements the heap already holds.
    // Every later block is written when it is allocated, but a heap populated before the renderer existed has no such event.
    // The published elements are consecutive, so the whole range is one descriptor write per frame rather than one driver call per block.
    const SceneGpuLayout::BindingSpec* vertex_table =
        SceneGpuLayout::TableBinding(SceneGpuLayout::TableId::vertex);
    if (vh.GetBufferCount() != 0) {
        std::vector<vk::DescriptorBufferInfo> infos;
        infos.reserve(vh.GetBufferCount());
        for (std::uint32_t block = 0; block < vh.GetBufferCount(); ++block) {
            infos.emplace_back(vh.GetBuffer(block), 0, vk::WholeSize);
        }
        for (auto& fr : frames_) {
            fr[SetIdx::vertex_buffers].UpdateBuffers(vertex_table->binding,
                                                     vertex_table->kind, infos);
        }
    }

    return true;
}

void SceneRenderer::WriteBinding(FrameResources& fr, const SceneGpuLayout::SetSpec& set,
                                 const SceneGpuLayout::BindingSpec& binding) {
    if (!backend_) return;
    GpuResources::GpuDescriptorSet& dst = SetFor(fr, set.id);
    if (!dst.IsValid()) return;

    const std::uint32_t row = SceneGpuLayout::WrittenBindingIndex(set.id, binding.binding);
    if (row == SceneGpuLayout::kNoBinding) return;
    FrameResources::WriteCursor& cursor = fr.write_cursors[row];
    const bool rewritten = cursor.written_epoch != binding_epoch_;

    if (const auto* blocks = std::get_if<SceneGpuLayout::BlockArraySource>(&binding.source)) {
        GpuResources::BlockArray& array = fr[blocks->slot];
        const std::uint32_t block_count = array.BlockCount();
        const std::uint32_t first = rewritten ? 0u : cursor.written_blocks;
        if (first >= block_count) {
            cursor.written_epoch = binding_epoch_;
            return;
        }
        // Only the blocks that do not exist yet: blocks are appended and never removed, and a new binding epoch (the buffers were re-created) rewrites the whole array.
        // Consecutive array elements take one write, so a fresh array costs one driver call rather than one per block.
        std::vector<vk::DescriptorBufferInfo> infos;
        infos.reserve(block_count - first);
        for (std::uint32_t block = first; block < block_count; ++block) {
            infos.emplace_back(array.GetBlockArray(block), 0, array.BlockSize());
        }
        dst.UpdateBuffers(binding.binding, binding.kind, infos, first);
        cursor.written_blocks = block_count;
        cursor.written_epoch = binding_epoch_;
        return;
    }

    if (const auto* buffers = std::get_if<SceneGpuLayout::BufferSource>(&binding.source)) {
        // A buffer binding points at one object for the life of that object, so it is written once per epoch rather than every frame.
        if (!rewritten) return;
        GpuResources::GpuBuffer& buffer =
            BufferRef(fr, draw_mode_ == DrawMode::MID ? buffers->mid : buffers->cid);
        // A mode with no buffer for this slot leaves the descriptor unwritten; the epoch stays stale so the next frame retries, and a mode switch re-creates the buffers anyway.
        if (!buffer.IsValid()) return;
        const std::array<vk::DescriptorBufferInfo, 1> info{
            vk::DescriptorBufferInfo{*buffer.GetBuffer(), 0, vk::WholeSize}};
        dst.UpdateBuffers(binding.binding, binding.kind, info);
        cursor.written_epoch = binding_epoch_;
        return;
    }

    if (std::holds_alternative<SceneGpuLayout::HizImageSource>(binding.source)) {
        // Both handles come from the Hi-Z ring, so a headless frame (which never created it) leaves the binding alone and retries once the ring exists.
        if (!rewritten || !hiz_sampler_ || !*fr.hiz_full_view) return;
        const std::array<vk::DescriptorImageInfo, 1> info{vk::DescriptorImageInfo{
            **hiz_sampler_, *fr.hiz_full_view, vk::ImageLayout::eShaderReadOnlyOptimal}};
        dst.UpdateImages(binding.binding, binding.kind, info);
        cursor.written_epoch = binding_epoch_;
        return;
    }
}

void SceneRenderer::WriteHiZBindings(FrameResources& fr) {
    if (!backend_ || !hiz_sampler_ || !*fr.hiz_image || hiz_mip_count_ == 0) return;
    GpuResources::GpuDescriptorSet& dst = fr[SetIdx::hiz];
    if (!dst.IsValid()) return;

    // The hiz set carries one binding of each kind, so the row lookup is the only place those numbers live.
    const std::uint32_t depth_binding = SceneGpuLayout::BindingOfKind(
        SceneGpuLayout::SetId::hiz, vk::DescriptorType::eSampledImage);
    const std::uint32_t sampler_binding = SceneGpuLayout::BindingOfKind(
        SceneGpuLayout::SetId::hiz, vk::DescriptorType::eSampler);
    const std::uint32_t levels_binding = SceneGpuLayout::BindingOfKind(
        SceneGpuLayout::SetId::hiz, vk::DescriptorType::eStorageImage);
    assert(depth_binding != SceneGpuLayout::kNoBinding &&
           sampler_binding != SceneGpuLayout::kNoBinding &&
           levels_binding != SceneGpuLayout::kNoBinding);

    // Bind the whole storage-image array the layout declares, re-pointing the tail levels (beyond mip_levels) at the coarsest mip so every descriptor stays valid after a shrink; leaving them would dangle at the previous resize's views.
    std::vector<vk::DescriptorImageInfo> storage_infos;
    storage_infos.reserve(SceneLimits::kMaxHiZMips);
    const std::uint32_t last_level = hiz_mip_count_ - 1;
    for (std::uint32_t mip = 0; mip < SceneLimits::kMaxHiZMips; ++mip) {
        const std::uint32_t view = std::min(mip, last_level);
        storage_infos.emplace_back(nullptr, *fr.hiz_mip_views[view], vk::ImageLayout::eGeneral);
    }
    dst.UpdateImages(levels_binding, vk::DescriptorType::eStorageImage, storage_infos);

    // Bind hiz mip 0 as the placeholder depth input; eGeneral matches the storage-image binding on the same subresource.
    const std::array<vk::DescriptorImageInfo, 1> depth_info{
        vk::DescriptorImageInfo{nullptr, *fr.hiz_mip_views[0], vk::ImageLayout::eGeneral}};
    dst.UpdateImages(depth_binding, vk::DescriptorType::eSampledImage, depth_info);

    const std::array<vk::DescriptorImageInfo, 1> sampler_info{
        vk::DescriptorImageInfo{**hiz_sampler_, nullptr, vk::ImageLayout::eShaderReadOnlyOptimal}};
    dst.UpdateImages(sampler_binding, vk::DescriptorType::eSampler, sampler_info);
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
        // MaxBlocksForDevice already floors at 1 and starts at kMaxBlocksCeiling.
        max_blocks_ = SceneGpuLayout::MaxBlocksForDevice(device_limits);
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

    // Size every pipeline retire ring consistently with the frame depth, one row per built-in pass plus the depth prepass.
    for (const ComputePipelineRow& row : ComputePipelineRows()) {
        (this->*row.slot).SetFramesInFlight(frames_in_flight_);
    }
    depth_slot_.SetFramesInFlight(frames_in_flight_);

    // Descriptor sets: layouts, pools and the per-frame sets.
    // Every binding number, descriptor count, stage mask, binding flag and pool size comes from SceneGpuLayout::kSceneSets, so a set's shape is declared exactly once.
    if (!CreateDescriptorSets(vh)) return false;

    // Create all compute/graphics pipelines in one batched submission
    if (!CreateScenePipelines(be, shader_mgr, pipeline_factory, shader_ids)) return false;

    // Per-frame ring resources.
    {
        // The singleton buffers outlive the ring: the shared technique flag table and the lighting header.
        for (const SceneGpuLayout::SingletonBufferSpec& spec : SceneGpuLayout::kSingletonBuffers) {
            GpuResources::GpuBuffer& buffer = owned_buffers_[spec.slot];
            buffer = GpuResources::GpuBuffer::Create(be, BufferBytes(spec.size),
                                                    spec.flags.usage, spec.flags.memory);
            VulkanBackend::Vulkan::SetVulkanObjectName(dev, buffer.GetBuffer(),
                                                       vk::ObjectType::eBuffer,
                                                       std::string(spec.name));
        }

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
                cfg.memory_mode = ToMemoryMode(spec.memory);
                fr[spec.slot].Initialize(be, cfg);
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

    // ── Lighting system: the engine-side scene_uniform set ──
    //
    // The set layout, pool and header descriptor come from the rows; the light block array's geometry is stated here because it is sized by the Light record rather than by a SceneCapacity field.
    {
        GpuResources::BlockArray::Config light_cfg{};
        light_cfg.entry_size = static_cast<std::uint32_t>(sizeof(Light));
        light_cfg.entries_per_block = SceneLimits::kLightsPerBlock;
        light_cfg.max_blocks = SceneLimits::kMaxLightBlocks;
        light_cfg.memory_mode = GpuResources::MemoryMode::DeviceLocal;
        light_cfg.memory = vk::MemoryPropertyFlagBits::eDeviceLocal;
        light_cfg.extra_usage = vk::BufferUsageFlagBits::eTransferDst;
        scene_lights.Initialize(be, light_cfg);
        light_blocks_written_ = 0;
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

    // Any descriptor written against the previous buffers is stale, so force a full rewrite on the next PrepareCompute.
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
            GpuResources::GpuBuffer& buffer = fr[spec.slot];
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
            fr[spec.slot] = GpuResources::GpuBuffer{};
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
    mip_levels = std::min(mip_levels, SceneLimits::kMaxHiZMips);
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

    // The ring just replaced every image view the occlusion set samples, so a descriptor written against the previous ring is as stale as one written against a previous buffer.
    // Bumping the epoch makes the next PrepareCompute republish it, the same mechanism CreateFrameBuffers uses for buffers.
    ++binding_epoch_;

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
    // One row per built-in compute pass, the same table CreateScenePipelines builds from.
    for (const ComputePipelineRow& row : ComputePipelineRows()) {
        poll(this->*row.slot, shader_ids_.*row.shader, this->*row.desc);
    }
    // The depth prepass is the one graphics pipeline the renderer owns, so it has no compute row.
    poll_graphics(depth_slot_, shader_ids_.depth_indir_vert, shader_ids_.depth_prepass_frag, depth_desc_);
}

void SceneRenderer::Shutdown() {
    for (auto& fr : frames_) {
        for (const SceneGpuLayout::BlockArraySpec& spec : SceneGpuLayout::kBlockArrays) {
            fr[spec.slot].Shutdown();
        }
        for (const SceneGpuLayout::BufferSpec& spec : SceneGpuLayout::kFrameBuffers) {
            fr[spec.slot] = GpuResources::GpuBuffer{};
        }
        for (const SceneGpuLayout::SetSpec& set : SceneGpuLayout::kSceneSets) {
            if (set.fixed_sets != 0) continue;   // the lighting set is not ringed
            fr[set.id] = GpuResources::GpuDescriptorSet{};
        }
        fr.write_cursors.fill(FrameResources::WriteCursor{});
    }
    frame_modifier_dispatches_.clear();
    has_active_deform_ = false;
    // Pipelines reference the layouts, so drop them before the sets.
    modifier_pipelines_.clear();
    modifier_registry_version_ = 0;
    layouts_ = {};
    modifier_mgr_ = nullptr;
    scene_lights.Shutdown();
    light_blocks_written_ = 0;
    for (const SceneGpuLayout::SingletonBufferSpec& spec : SceneGpuLayout::kSingletonBuffers) {
        owned_buffers_[spec.slot] = GpuResources::GpuBuffer{};
    }
    technique_flags_cache_.clear();
    for (const SceneGpuLayout::SetSpec& set : SceneGpuLayout::kSceneSets) {
        if (set.fixed_sets == 0) continue;
        owned_sets_[set.id] = GpuResources::GpuDescriptorSet{};
    }
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
    return LayoutPointer(sets_[SceneGpuLayout::SetId::submesh_vertex].layout);
}

vk::DescriptorSetLayout* SceneRenderer::GetVertexBuffersLayout() const {
    return LayoutPointer(sets_[SceneGpuLayout::SetId::vertex_buffers].layout);
}

vk::DescriptorSetLayout* SceneRenderer::GetIndirectionLayout() const {
    return LayoutPointer(sets_[SceneGpuLayout::SetId::indirection].layout);
}

void SceneRenderer::UpdateTableElement(const SceneGpuLayout::TableId table,
                                       const std::optional<std::uint32_t> frame,
                                       const std::uint32_t index,
                                       const vk::Buffer buffer,
                                       const std::uint64_t size) {
    if (!backend_ || frames_.empty()) return;

    // The bound is the row's own descriptor count, so a table can never be written past its array.
    const SceneGpuLayout::BindingSpec& row = *SceneGpuLayout::TableBinding(table);
    const std::uint32_t capacity = SceneGpuLayout::DescriptorCount(row, max_blocks_);
    if (index >= capacity) {
        static bool reported = false;
        if (!reported) {
            reported = true;
            LOGIFACE_LOG(error, "UpdateTableElement: element " + std::to_string(index) +
                " is past the " + std::to_string(capacity) + "-element table of set '" +
                std::string(SceneGpuLayout::SetOf(SceneGpuLayout::TableOf(table).set).name) +
                "' binding " + std::to_string(row.binding) +
                "; the descriptor is not written");
        }
        return;
    }
    if (!buffer) {
        LOGIFACE_LOG(warn, "UpdateTableElement: null buffer for element " + std::to_string(index));
        return;
    }

    const std::array<vk::DescriptorBufferInfo, 1> info{vk::DescriptorBufferInfo{buffer, 0, size}};
    const auto write_one = [&](GpuResources::GpuDescriptorSet& dst) {
        dst.UpdateBuffers(row.binding, row.kind, info, index);
    };
    const SceneGpuLayout::SetId set = SceneGpuLayout::TableOf(table).set;
    // A set that is not ringed owns one set, so "every frame" and "this frame" are the same write.
    if (SceneGpuLayout::SetOf(set).fixed_sets != 0) {
        write_one(owned_sets_[set]);
        return;
    }
    if (frame.has_value()) {
        write_one(frames_[*frame % frames_in_flight_][set]);
        return;
    }
    for (auto& fr : frames_) write_one(fr[set]);
}

GpuResources::BlockArray& SceneRenderer::GetFrameSkinEntries(std::uint32_t frame_index) {
    return frames_[frame_index % frames_in_flight_][BlkIdx::skin_entries];
}

std::span<std::array<float, 16>> SceneRenderer::MapFrameBonePalettes(std::uint32_t frame_index) {
    if (!backend_ || frames_.empty()) return {};
    auto& buffer = frames_[frame_index % frames_in_flight_][BufIdx::bone_palettes];
    if (!buffer.IsValid()) return {};
    const std::uint64_t bytes = buffer.GetSize();
    auto* mapped = static_cast<std::array<float, 16>*>(buffer.Map(0, bytes));
    if (mapped == nullptr) return {};
    return {mapped, static_cast<std::size_t>(bytes / (16u * sizeof(float)))};
}

void SceneRenderer::UnmapFrameBonePalettes(std::uint32_t frame_index) {
    if (!backend_ || frames_.empty()) return;
    auto& buffer = frames_[frame_index % frames_in_flight_][BufIdx::bone_palettes];
    if (buffer.IsValid()) buffer.Unmap();
}

std::span<std::byte> SceneRenderer::MapFrameModifierParams(std::uint32_t frame_index) {
    if (!backend_ || frames_.empty()) return {};
    auto& buffer = frames_[frame_index % frames_in_flight_][BufIdx::modifier_params];
    if (!buffer.IsValid()) return {};
    const std::uint64_t bytes = buffer.GetSize();
    auto* mapped = static_cast<std::byte*>(buffer.Map(0, bytes));
    if (mapped == nullptr) return {};
    return {mapped, static_cast<std::size_t>(bytes)};
}

void SceneRenderer::UnmapFrameModifierParams(std::uint32_t frame_index) {
    if (!backend_ || frames_.empty()) return;
    auto& buffer = frames_[frame_index % frames_in_flight_][BufIdx::modifier_params];
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
        !layouts_[SceneGpuLayout::PipelineLayoutId::deform]) {
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
            pipeline_desc.layout = Layout(SceneGpuLayout::PipelineLayoutId::deform);
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
    const auto& buffer = frames_[frame_index % frames_in_flight_][BufIdx::deform_vertices];
    return buffer.IsValid() ? *buffer.GetBuffer() : vk::Buffer{};
}

VulkanEngine::GpuResources::BlockArray& SceneRenderer::GetFrameVertexEntries(
    const std::uint32_t frame_index) {
    return frames_[frame_index % frames_in_flight_][BlkIdx::submesh_vertex_entries];
}

VulkanEngine::GpuResources::BlockArray& SceneRenderer::GetFrameCullEntries(
    const std::uint32_t frame_index) {
    return frames_[frame_index % frames_in_flight_][BlkIdx::cull_entries];
}

void SceneRenderer::UpdateHizDepthBinding(std::uint32_t frame_index, vk::ImageView depth_view) {
    if (!backend_ || frames_.empty()) return;
    const std::uint32_t binding = SceneGpuLayout::BindingOfKind(
        SceneGpuLayout::SetId::hiz, vk::DescriptorType::eSampledImage);
    if (binding == SceneGpuLayout::kNoBinding || !depth_view) return;
    const std::array<vk::DescriptorImageInfo, 1> info{vk::DescriptorImageInfo{
        nullptr, depth_view, vk::ImageLayout::eShaderReadOnlyOptimal}};
    frames_[frame_index % frames_in_flight_][SetIdx::hiz].UpdateImages(
        binding, vk::DescriptorType::eSampledImage, info);
}

SceneRenderer::FrameBlockArrays SceneRenderer::GetFrameBlockArrays(std::uint32_t frame_index) {
    auto& fr = frames_[frame_index % frames_in_flight_];
    return {
        &fr[BlkIdx::dynamic_entries],
        &fr[BlkIdx::static_entries],
        &fr[BlkIdx::bounding_spheres],
        &fr[BlkIdx::obb_entries]
    };
}

void SceneRenderer::UploadLighting(const SceneHeader& header,
                                    std::span<const Light> lights,
                                    GpuResources::StagingPool& staging) {
    if (!backend_) return;

    // Grow the block array to fit all lights.
    // The descriptor binding is a fixed kMaxLightBlocks array, so the upload is clamped to what actually fits.
    constexpr std::size_t kLightCapacity =
        static_cast<std::size_t>(SceneLimits::kMaxLightBlocks) * SceneLimits::kLightsPerBlock;
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

    // Stage the header buffer.
    {
        auto header_slice = staging.Allocate(sizeof(SceneHeader));
        if (header_slice.has_value()) {
            std::memcpy(header_slice->mapped_ptr, &header, sizeof(SceneHeader));
            staging.RecordBufferCopy(*header_slice,
                                     static_cast<vk::Buffer>(
                                         *owned_buffers_[BufIdx::scene_header].GetBuffer()),
                                     0);
        }
    }

    // Stage each light through BlockArray::UploadEntry (which uses the staging pool internally).
    for (std::size_t i = 0; i < uploadable; ++i) {
        scene_lights.UploadEntry(static_cast<std::uint32_t>(i), &lights[i],
                                 sizeof(Light), staging);
    }

    // Publish only the blocks that do not exist yet: the array only ever grows, so a published block never changes its buffer.
    for (std::uint32_t bi = light_blocks_written_; bi < scene_lights.BlockCount(); ++bi) {
        UpdateTableElement(SceneGpuLayout::TableId::light, std::nullopt, bi,
                           scene_lights.GetBlockArray(bi), scene_lights.BlockSize());
    }
    light_blocks_written_ = scene_lights.BlockCount();

    staging.FlushImmediate();
}

} // namespace VulkanEngine::SceneRenderer
