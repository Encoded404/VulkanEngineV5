module;

#include <cassert>
#include <logging/logging_macros.hpp>

module VulkanEngine.TechniqueManager.BaseTechnique;

import std;
import std.compat;

import logiface;

import vulkan_hpp;

import VulkanBackend.Vulkan.VulkanBootstrap;
import VulkanBackend.Vulkan.VulkanDebugUtils;
import VulkanEngine.StandardMeshPipeline;
import VulkanEngine.GpuResources.BlockArray;
import VulkanEngine.GpuBuffer;
import VulkanEngine.GpuResources.StagingManager;

namespace {
    VulkanEngine::TechniqueManager::TechniqueId s_next_technique_id{0};

    constexpr uint32_t kTechniqueBits  = 12;
    constexpr uint32_t kTechniqueMask  = (1u << kTechniqueBits) - 1;

    template<typename Handle>
    std::uint64_t HandleToU64(Handle h) {
        return reinterpret_cast<std::uint64_t>(static_cast<typename Handle::CType>(h));
    }
}

namespace VulkanEngine::TechniqueManager {

void BaseTechnique::Shutdown() {
    custom_descriptor_sets_.clear();
    custom_descriptor_set_handles_.clear();
    descriptor_pool_ = nullptr;
    custom_set_layouts_.clear();
    block_arrays_.clear();
    shared_buffers_.clear();
    shared_cpu_data_.clear();
    pipeline_layout_ = nullptr;
    bindings_.clear();
}

uint32_t BaseTechnique::PackMaterialData(uint32_t material_id) const {
    // Default: pack material_id and technique_id into a single uint32
    return (material_id << kTechniqueBits) | (id_.value & kTechniqueMask);
}

void BaseTechnique::ValidateNoBindingCollision(std::uint32_t set, std::uint32_t binding) const {
    for (const auto& decl : bindings_) {
        assert(!(decl.set == set && decl.binding == binding) &&
               "Binding collision: set+binding already declared in this technique");
        if (decl.set == set && decl.binding == binding) break;
    }
}

void BaseTechnique::DeclareBindingImpl(BindingDecl decl) {
    if (id_.value == 0) {
        id_ = s_next_technique_id;
        s_next_technique_id = TechniqueId{static_cast<std::uint16_t>(s_next_technique_id.value + 1)};
    }
    bindings_.push_back(std::move(decl));
}

std::vector<BaseTechnique::BindingGroup> BaseTechnique::GroupBindingsBySet() const {
    // Group custom bindings (set >= 5) by set number
    std::unordered_map<std::uint32_t, BindingGroup> group_map;
    for (const auto& decl : bindings_) {
        if (decl.set >= 5) {
            auto it = group_map.find(decl.set);
            if (it == group_map.end()) {
                BindingGroup bg{decl.set, {}};
                bg.bindings.push_back(&decl);
                group_map[decl.set] = std::move(bg);
            } else {
                it->second.bindings.push_back(&decl);
            }
        }
    }

    // Sort by set number for deterministic layout
    std::vector<BindingGroup> groups;
    groups.reserve(group_map.size());
    for (auto &group: group_map | std::views::values) {
        groups.push_back(std::move(group));
    }
    std::ranges::sort(groups,
                      [](const BindingGroup& a, const BindingGroup& b) { return a.set < b.set; });
    return groups;
}

void BaseTechnique::Compile(VulkanBackend::Vulkan::VulkanBootstrap& bootstrap,
                            ShaderSystem::ShaderManager& shader_mgr,
                            ShaderSystem::PipelineFactory& pipeline_factory,
                            ShaderSystem::ShaderId vert_id,
                            ShaderSystem::ShaderId frag_id,
                            const VulkanEngine::StandardMeshPipeline::PipelineConfig& config,
                            vk::DescriptorSetLayout bindless_layout,
                            vk::DescriptorSetLayout submesh_vertex_layout,
                            vk::DescriptorSetLayout raw_vertex_layout,
                            vk::DescriptorSetLayout indirection_layout,
                             vk::DescriptorSetLayout scene_uniform_layout) {
    const auto& device = bootstrap.GetBackend().GetDevice();
    LOGIFACE_LOG(debug, std::format("BaseTechnique: compiling technique (vert={}, frag={})",
                                    vert_id, frag_id));

    // ── 1. Build descriptor set layout array ──
    // Engine sets 0-4 are always at layout slots 0-4
    std::vector<vk::DescriptorSetLayout> set_layouts = {
        bindless_layout,           // set 0: bindless textures
        submesh_vertex_layout,     // set 1: submesh vertex data
        raw_vertex_layout,         // set 2: raw vertex buffers
        indirection_layout,        // set 3: indirection data
        scene_uniform_layout,      // set 4: scene uniforms (lighting, camera)
    };

    // ── 2. Group custom bindings by set number and create descriptor set layouts ──
    auto custom_groups = GroupBindingsBySet();
    std::vector<vk::raii::DescriptorSetLayout> custom_set_layouts;
    custom_set_layouts.reserve(custom_groups.size());

    for (const auto& group : custom_groups) {
        std::vector<vk::DescriptorSetLayoutBinding> vk_bindings;
        vk_bindings.reserve(group.bindings.size());

        for (const auto* decl : group.bindings) {
            vk::DescriptorType desc_type;
            if (decl->kind == BindingKind::PerMaterial) {
                // PerMaterial bindings use StorageBuffer (StructuredBuffer in HLSL)
                desc_type = vk::DescriptorType::eStorageBuffer;
            } else {
                // Shared bindings also use StorageBuffer
                desc_type = vk::DescriptorType::eStorageBuffer;
            }

            vk::DescriptorSetLayoutBinding binding{};
            binding.binding = decl->binding;
            binding.descriptorType = desc_type;
            binding.descriptorCount = 1;
            binding.stageFlags = vk::ShaderStageFlagBits::eVertex |
                                 vk::ShaderStageFlagBits::eFragment;
            vk_bindings.push_back(binding);
        }

        vk::DescriptorSetLayoutCreateInfo layout_info{};
        layout_info.bindingCount = static_cast<std::uint32_t>(vk_bindings.size());
        layout_info.pBindings = vk_bindings.data();

        custom_set_layouts.emplace_back(device, layout_info);
        set_layouts.push_back(*custom_set_layouts.back());
    }

    // ── 3. Determine push constant ranges ──
    // Camera position (fragment stage, 16 bytes at offset 0).
    std::vector<vk::PushConstantRange> push_constant_ranges;
    push_constant_ranges.emplace_back(
        vk::ShaderStageFlagBits::eFragment, 0, 16);

    // ── 4. Create VkPipelineLayout ──
    vk::PipelineLayoutCreateInfo layout_info{};
    layout_info.setLayoutCount = static_cast<std::uint32_t>(set_layouts.size());
    layout_info.pSetLayouts = set_layouts.data();
    layout_info.pushConstantRangeCount = static_cast<std::uint32_t>(push_constant_ranges.size());
    layout_info.pPushConstantRanges = push_constant_ranges.data();

    pipeline_layout_ = vk::raii::PipelineLayout(device, layout_info);
    VulkanBackend::Vulkan::SetVulkanObjectName(device, pipeline_layout_, "base-technique-layout");

    // ── 5. Create pipeline via PipelineFactory ──
    {
        const vk::Format surface_format = bootstrap.GetBackend().GetSurfaceFormat().format;
        const vk::Format depth_format = bootstrap.GetBackend().GetDepthFormat();

        ShaderSystem::GraphicsPipelineDesc desc{};
        desc.vertex_shader = vert_id;
        desc.fragment_shader = frag_id;
        desc.vertex_input = vk::PipelineVertexInputStateCreateInfo({}, 0, nullptr, 0, nullptr);
        desc.input_assembly = vk::PipelineInputAssemblyStateCreateInfo({}, config.primitive_topology);
        desc.viewport = vk::PipelineViewportStateCreateInfo({}, 1, nullptr, 1, nullptr);
        desc.rasterization = vk::PipelineRasterizationStateCreateInfo({}, false, false, config.polygon_mode, config.cull_mode, config.front_face, false, 0, 0, 0, config.line_width);
        desc.multisample = vk::PipelineMultisampleStateCreateInfo({}, config.sample_count);
        desc.depth_stencil = vk::PipelineDepthStencilStateCreateInfo({}, config.depth_test_enable, config.depth_write_enable, config.depth_compare_op);
        const vk::PipelineColorBlendAttachmentState color_blend_attachment(
            config.blend_enable,
            config.src_color_blend_factor, config.dst_color_blend_factor, config.color_blend_op,
            config.src_alpha_blend_factor, config.dst_alpha_blend_factor, config.alpha_blend_op,
            vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA);
        desc.color_blend = vk::PipelineColorBlendStateCreateInfo({}, false, vk::LogicOp::eCopy, color_blend_attachment);
        desc.dynamic_states = { vk::DynamicState::eViewport, vk::DynamicState::eScissor };
        desc.layout = *pipeline_layout_;
        desc.color_formats = { surface_format };
        desc.depth_format = depth_format;

        auto result = pipeline_factory.CreateGraphics(desc, shader_mgr);
        if (!result.has_value()) {
            LOGIFACE_LOG(error, "BaseTechnique: pipeline creation failed");
        } else {
            pipeline_slot_.Swap(std::move(result.value()), 0);
            VulkanBackend::Vulkan::SetVulkanObjectName(device, pipeline_slot_.Get(), vk::ObjectType::ePipeline, "technique-pipeline");
            LOGIFACE_LOG(debug, std::format("BaseTechnique: pipeline ready: 0x{:x} (layout 0x{:x})",
                                            HandleToU64(pipeline_slot_.Get()),
                                            HandleToU64(*pipeline_layout_)));
        }
    }

    // ── 6. Create BlockArrays for PerMaterial bindings ──
    block_arrays_.clear();
    block_arrays_.reserve(bindings_.size());
    for (const auto& decl : bindings_) {
        if (decl.kind == BindingKind::PerMaterial) {
            VulkanEngine::GpuResources::BlockArray ba;
            VulkanEngine::GpuResources::BlockArray::Config ba_cfg{};
            ba_cfg.entry_size = decl.stride;
            ba_cfg.entries_per_block = 256;
            ba_cfg.memory = vk::MemoryPropertyFlagBits::eDeviceLocal;
            ba_cfg.memory_mode = VulkanEngine::GpuResources::MemoryMode::DeviceLocal;
            if (!ba.Initialize(bootstrap.GetBackend(), ba_cfg)) {
                LOGIFACE_LOG(error, "BaseTechnique: Failed to initialize BlockArray for PerMaterial binding");
            }
            block_arrays_.push_back(std::move(ba));
        }
    }

    // ── 7. Create GpuBuffers for Shared bindings ──
    shared_buffers_.clear();
    shared_cpu_data_.clear();
    shared_buffers_.reserve(bindings_.size());
    shared_cpu_data_.reserve(bindings_.size());
    for (const auto& decl : bindings_) {
        if (decl.kind == BindingKind::Shared) {
            // Create device-local storage buffer
            // Shared data is small (typically < 256 bytes), but we allocate a reasonable size
            constexpr std::uint64_t SHARED_BUFFER_SIZE = 256;
            auto buf = VulkanEngine::GpuResources::GpuBuffer::Create(
                bootstrap.GetBackend(),
                SHARED_BUFFER_SIZE,
                vk::BufferUsageFlagBits::eStorageBuffer |
                vk::BufferUsageFlagBits::eTransferDst,
                vk::MemoryPropertyFlagBits::eDeviceLocal);
            if (!buf.IsValid()) {
                LOGIFACE_LOG(error, "BaseTechnique: Failed to create shared buffer");
            }
            shared_buffers_.push_back(std::move(buf));

            // Allocate technique-local CPU buffer
            shared_cpu_data_.emplace_back(decl.stride > 0 ? decl.stride : 64, std::byte{0});
        }
    }

    // ── 8. Store custom set layouts as members (fixes lifetime bug — were local in Step 2) ──
    custom_set_layouts_ = std::move(custom_set_layouts);

    // ── 9. Create descriptor pool and sets for custom bindings ──
    // Pre-allocate at least 1 block in each PerMaterial BlockArray so descriptors point to valid memory.
    for (auto& ba : block_arrays_) {
        ba.EnsureCapacity(1);
    }

    // Calculate total descriptor count needed
    std::uint32_t total_descriptors = 0;
    for (const auto& [set, bindings] : custom_groups) {
        total_descriptors += static_cast<std::uint32_t>(bindings.size());
    }

    if (total_descriptors > 0) {
        const vk::DescriptorPoolSize pool_size(
            vk::DescriptorType::eStorageBuffer, total_descriptors);
        const vk::DescriptorPoolCreateInfo pool_info(
            vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,
            total_descriptors, pool_size);
        descriptor_pool_ = vk::raii::DescriptorPool(device, pool_info);

        custom_descriptor_sets_.clear();
        custom_descriptor_sets_.reserve(custom_groups.size());
        custom_descriptor_set_handles_.clear();
        custom_descriptor_set_handles_.reserve(custom_groups.size());

        for (std::size_t gi = 0; gi < custom_groups.size(); ++gi) {
            const vk::DescriptorSetAllocateInfo alloc_info(
                *descriptor_pool_, *custom_set_layouts_[gi]);
            std::vector<vk::raii::DescriptorSet> ds_vector = device.allocateDescriptorSets(alloc_info);
            vk::raii::DescriptorSet ds = std::move(ds_vector[0]);
            const vk::DescriptorSet raw_ds = *ds;

            for (const auto* decl : custom_groups[gi].bindings) {
                // Find the index in bindings_ matching this decl
                const std::size_t binding_idx = static_cast<std::size_t>(decl - bindings_.data());
                vk::DescriptorBufferInfo buf_info;

                if (decl->kind == BindingKind::PerMaterial) {
                    // Count PerMaterial bindings up to binding_idx to find block_arrays_ index
                    std::size_t pm_count = 0;
                    for (std::size_t i = 0; i < binding_idx; ++i) {
                        if (bindings_[i].kind == BindingKind::PerMaterial) ++pm_count;
                    }
                    buf_info = vk::DescriptorBufferInfo(
                        block_arrays_[pm_count].GetBlockArray(0), 0,
                        block_arrays_[pm_count].BlockSize());
                } else /* Shared */ {
                    // Count Shared bindings up to binding_idx to find shared_buffers_ index
                    std::size_t sh_count = 0;
                    for (std::size_t i = 0; i < binding_idx; ++i) {
                        if (bindings_[i].kind == BindingKind::Shared) ++sh_count;
                    }
                    buf_info = vk::DescriptorBufferInfo(
                        *shared_buffers_[sh_count].GetBuffer(), 0, vk::WholeSize);
                }

                const vk::WriteDescriptorSet write(raw_ds, decl->binding, 0, 1,
                    vk::DescriptorType::eStorageBuffer, nullptr, &buf_info);
                device.updateDescriptorSets(write, nullptr);
            }

            custom_descriptor_set_handles_.push_back(raw_ds);
            custom_descriptor_sets_.push_back(std::move(ds));
        }
    }
}

} // namespace VulkanEngine::TechniqueManager
