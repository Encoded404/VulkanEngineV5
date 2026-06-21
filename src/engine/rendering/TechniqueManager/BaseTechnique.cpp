module;

#include <cassert>
#include <logging/logging_macros.hpp>

module VulkanEngine.TechniqueManager.BaseTechnique;

import std;
import std.compat;

import logiface;

import vulkan_hpp;

import VulkanBackend.Runtime.VulkanBootstrap;
import VulkanBackend.Utils.VulkanDebugUtils;
import VulkanEngine.StandardMeshPipeline;
import VulkanEngine.GpuResources.BlockArray;
import VulkanEngine.GpuBuffer;
import VulkanEngine.GpuResources.StagingManager;

namespace VulkanEngine::TechniqueManager {

static TechniqueId s_next_technique_id{0};

void BaseTechnique::Shutdown() {
    block_arrays_.clear();
    shared_buffers_.clear();
    shared_cpu_data_.clear();
    pipeline_ = nullptr;
    pipeline_layout_ = nullptr;
    bindings_.clear();
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
    // Group custom bindings (set >= 4) by set number
    std::unordered_map<std::uint32_t, BindingGroup> group_map;
    for (const auto& decl : bindings_) {
        if (decl.set >= 4) {
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
    for (auto& [set, group] : group_map) {
        groups.push_back(std::move(group));
    }
    std::sort(groups.begin(), groups.end(),
              [](const BindingGroup& a, const BindingGroup& b) { return a.set < b.set; });
    return groups;
}

void BaseTechnique::Compile(VulkanEngine::Runtime::VulkanBootstrap& bootstrap,
                            std::span<const std::uint32_t> vert_spv,
                            std::span<const std::uint32_t> frag_spv,
                            const VulkanEngine::StandardMeshPipeline::PipelineConfig& config,
                            vk::DescriptorSetLayout bindless_layout,
                            vk::DescriptorSetLayout submesh_vertex_layout,
                            vk::DescriptorSetLayout raw_vertex_layout,
                            vk::DescriptorSetLayout indirection_layout) {
    const auto& device = bootstrap.GetBackend().GetDevice();

    // ── 1. Build descriptor set layout array ──
    // Engine sets 0-3 are always at layout slots 0-3
    std::vector<vk::DescriptorSetLayout> set_layouts = {
        bindless_layout,           // set 0: bindless textures
        submesh_vertex_layout,     // set 1: submesh vertex data
        raw_vertex_layout,         // set 2: raw vertex buffers
        indirection_layout,        // set 3: indirection data
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
    // Default: 64 bytes, vertex-only (matching existing behavior)
    std::vector<vk::PushConstantRange> push_constant_ranges;
    push_constant_ranges.push_back(
        vk::PushConstantRange(vk::ShaderStageFlagBits::eVertex, 0, 64));

    // ── 4. Create VkPipelineLayout ──
    vk::PipelineLayoutCreateInfo layout_info{};
    layout_info.setLayoutCount = static_cast<std::uint32_t>(set_layouts.size());
    layout_info.pSetLayouts = set_layouts.data();
    layout_info.pushConstantRangeCount = static_cast<std::uint32_t>(push_constant_ranges.size());
    layout_info.pPushConstantRanges = push_constant_ranges.data();

    pipeline_layout_ = std::make_unique<vk::raii::PipelineLayout>(device, layout_info);
    VulkanEngine::Utils::SetVulkanObjectName(device, *pipeline_layout_, "base-technique-layout");

    // ── 5. Create pipeline directly (reusing GraphicsPipeline's pipeline creation pattern) ──
    {
        const vk::ShaderModuleCreateInfo vert_info({}, vert_spv.size() * sizeof(std::uint32_t), vert_spv.data());
        const vk::raii::ShaderModule vert_module(device, vert_info);

        const vk::ShaderModuleCreateInfo frag_info({}, frag_spv.size() * sizeof(std::uint32_t), frag_spv.data());
        const vk::raii::ShaderModule frag_module(device, frag_info);

        std::array<vk::PipelineShaderStageCreateInfo, 2> stages = {
            vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eVertex, *vert_module, "main"),
            vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eFragment, *frag_module, "main")
        };

        // Use SSBO pulling (no vertex bindings)
        constexpr vk::PipelineVertexInputStateCreateInfo vertex_input({}, 0, nullptr, 0, nullptr);
        const vk::PipelineInputAssemblyStateCreateInfo input_assembly({}, config.primitive_topology);
        constexpr vk::PipelineViewportStateCreateInfo viewport_state({}, 1, nullptr, 1, nullptr);
        const vk::PipelineRasterizationStateCreateInfo rasterization({}, false, false, config.polygon_mode, config.cull_mode, config.front_face, false, 0, 0, 0, config.line_width);
        const vk::PipelineMultisampleStateCreateInfo multisample({}, config.sample_count);
        const vk::PipelineDepthStencilStateCreateInfo depth_stencil({}, config.depth_test_enable, config.depth_write_enable, config.depth_compare_op);

        const vk::PipelineColorBlendAttachmentState color_blend_attachment(
            config.blend_enable,
            config.src_color_blend_factor, config.dst_color_blend_factor, config.color_blend_op,
            config.src_alpha_blend_factor, config.dst_alpha_blend_factor, config.alpha_blend_op,
            vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA);
        const vk::PipelineColorBlendStateCreateInfo color_blend({}, false, vk::LogicOp::eCopy, color_blend_attachment);

        std::array<vk::DynamicState, 2> dynamic_states = {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
        const vk::PipelineDynamicStateCreateInfo dynamic_state({}, dynamic_states);

        const vk::Format format = bootstrap.GetBackend().GetSurfaceFormat().format;
        vk::PipelineRenderingCreateInfo rendering_info{};
        rendering_info.colorAttachmentCount = 1;
        rendering_info.pColorAttachmentFormats = &format;
        rendering_info.depthAttachmentFormat = bootstrap.GetBackend().GetDepthFormat();
        rendering_info.stencilAttachmentFormat = vk::Format::eUndefined;

        vk::GraphicsPipelineCreateInfo pipeline_info({}, stages, &vertex_input, &input_assembly, nullptr, &viewport_state, &rasterization, &multisample, &depth_stencil, &color_blend, &dynamic_state, *pipeline_layout_, nullptr, 0, {}, 0);
        pipeline_info.setPNext(&rendering_info);

        pipeline_ = std::make_unique<vk::raii::Pipeline>(device, nullptr, pipeline_info);
        VulkanEngine::Utils::SetVulkanObjectName(device, *pipeline_, "technique-pipeline");
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
}

} // namespace VulkanEngine::TechniqueManager
