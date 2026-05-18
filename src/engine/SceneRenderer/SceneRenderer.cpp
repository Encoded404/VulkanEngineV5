module;

#include <algorithm>
#include <cstdint>
#include <vector>
#include <numbers>
#include <array>
#include <filesystem>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_RADIANS
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <vulkan/vulkan_raii.hpp>

#include <logging/logging.hpp>

module VulkanEngine.SceneRenderer;

import VulkanBackend.Component;
import VulkanBackend.Runtime.VulkanBootstrap;
import VulkanEngine.Components.Transform;
import VulkanEngine.Components.MeshReference;
import VulkanEngine.Components.Material;
import VulkanEngine.GpuResources;
import VulkanEngine.StandardMeshPipeline;
import VulkanEngine.TechniqueManager;
import VulkanEngine.BindlessManager;
import VulkanEngine.ShaderLoader;

namespace VulkanEngine::SceneRenderer {

namespace {

struct DrawEntity {
    const VulkanEngine::Components::Transform* transform = nullptr;
    const VulkanEngine::Components::MeshReference* mesh = nullptr;
    const VulkanEngine::Components::Material* material = nullptr;
};

struct ExpandPushConstants {
    uint32_t entity_count;
};

} // namespace

SceneRenderer::~SceneRenderer() {
    Shutdown();
}

bool SceneRenderer::Initialize(VulkanEngine::Runtime::IVulkanBootstrapBackend& backend,
                                uint32_t total_vertex_count) {
    backend_ = &backend;
    total_vertex_count_ = total_vertex_count;
    const auto& device = backend.GetDevice();

    LOGIFACE_LOG(info, "SceneRenderer initializing with " + std::to_string(total_vertex_count) + " vertices");

    const uint32_t vert_count = std::max(total_vertex_count, 1u);

    // ── Set 0 (instance data) descriptor layout ──
    {
        vk::DescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = vk::DescriptorType::eStorageBuffer;
        binding.descriptorCount = 1;
        binding.stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eCompute;

        vk::DescriptorSetLayoutCreateInfo info{};
        info.bindingCount = 1;
        info.pBindings = &binding;
        instance_data_layout_ = std::make_unique<vk::raii::DescriptorSetLayout>(device, info);

        VulkanEngine::GpuResources::DescriptorPoolConfig pool_config{};
        pool_config.max_sets = FRAMES_IN_FLIGHT;
        pool_config.max_storage_buffers = FRAMES_IN_FLIGHT;
        instance_pool_ = VulkanEngine::GpuResources::DescriptorPool::Create(backend, pool_config);
    }

    // ── Set 1 (expanded buffers) descriptor layout ──
    {
        std::array<vk::DescriptorSetLayoutBinding, 6> bindings{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = vk::DescriptorType::eStorageBuffer;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = vk::ShaderStageFlagBits::eCompute | vk::ShaderStageFlagBits::eVertex;

        bindings[1].binding = 1;
        bindings[1].descriptorType = vk::DescriptorType::eStorageBuffer;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = vk::ShaderStageFlagBits::eCompute | vk::ShaderStageFlagBits::eVertex;

        bindings[2].binding = 2;
        bindings[2].descriptorType = vk::DescriptorType::eStorageBuffer;
        bindings[2].descriptorCount = 1;
        bindings[2].stageFlags = vk::ShaderStageFlagBits::eCompute;

        bindings[3].binding = 3;
        bindings[3].descriptorType = vk::DescriptorType::eStorageBuffer;
        bindings[3].descriptorCount = 1;
        bindings[3].stageFlags = vk::ShaderStageFlagBits::eCompute;

        bindings[4].binding = 4;
        bindings[4].descriptorType = vk::DescriptorType::eStorageBuffer;
        bindings[4].descriptorCount = 1;
        bindings[4].stageFlags = vk::ShaderStageFlagBits::eCompute;

        bindings[5].binding = 5;
        bindings[5].descriptorType = vk::DescriptorType::eStorageBuffer;
        bindings[5].descriptorCount = 1;
        bindings[5].stageFlags = vk::ShaderStageFlagBits::eCompute;

        vk::DescriptorSetLayoutCreateInfo info{};
        info.bindingCount = static_cast<uint32_t>(bindings.size());
        info.pBindings = bindings.data();
        expanded_data_layout_ = std::make_unique<vk::raii::DescriptorSetLayout>(device, info);

        VulkanEngine::GpuResources::DescriptorPoolConfig pool_config{};
        pool_config.max_sets = FRAMES_IN_FLIGHT;
        pool_config.max_storage_buffers = FRAMES_IN_FLIGHT * 6;
        expanded_pool_ = VulkanEngine::GpuResources::DescriptorPool::Create(backend, pool_config);
    }

    // ── Set 2 (camera + mesh + raw vertex) descriptor layout ──
    {
        std::array<vk::DescriptorSetLayoutBinding, 3> bindings{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = vk::DescriptorType::eUniformBuffer;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = vk::ShaderStageFlagBits::eCompute | vk::ShaderStageFlagBits::eVertex;

        bindings[1].binding = 1;
        bindings[1].descriptorType = vk::DescriptorType::eStorageBuffer;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = vk::ShaderStageFlagBits::eCompute;

        bindings[2].binding = 2;
        bindings[2].descriptorType = vk::DescriptorType::eStorageBuffer;
        bindings[2].descriptorCount = 1;
        bindings[2].stageFlags = vk::ShaderStageFlagBits::eCompute;

        vk::DescriptorSetLayoutCreateInfo info{};
        info.bindingCount = static_cast<uint32_t>(bindings.size());
        info.pBindings = bindings.data();
        camera_data_layout_ = std::make_unique<vk::raii::DescriptorSetLayout>(device, info);

        VulkanEngine::GpuResources::DescriptorPoolConfig pool_config{};
        pool_config.max_sets = FRAMES_IN_FLIGHT;
        pool_config.max_uniform_buffers = FRAMES_IN_FLIGHT;
        pool_config.max_storage_buffers = FRAMES_IN_FLIGHT * 2;
        camera_pool_ = VulkanEngine::GpuResources::DescriptorPool::Create(backend, pool_config);
    }

    // ── Expand compute pipeline ──
    if (!CreateExpandPipeline(backend)) {
        return false;
    }

    // ── Depth pre-pass pipeline ──
    if (!CreateDepthPipeline(backend)) {
        return false;
    }

    // ── Per-frame resources ──
    constexpr uint64_t instance_data_size = MAX_ENTITIES * sizeof(InstanceData);
    constexpr uint64_t indirect_data_size = MAX_ENTITIES * sizeof(VkDrawIndexedIndirectCommand);
    constexpr uint64_t mesh_data_size = MAX_ENTITIES * sizeof(MeshData);
    constexpr uint64_t camera_ubo_size = sizeof(CameraUBO);
    constexpr uint64_t counter_size = sizeof(uint32_t);

    const uint64_t expanded_position_size = static_cast<uint64_t>(vert_count) * sizeof(glm::vec4);
    const uint64_t expanded_attribute_size = static_cast<uint64_t>(vert_count) * sizeof(glm::uvec4);
    const uint64_t expanded_index_size = static_cast<uint64_t>(vert_count) * sizeof(uint32_t);

    LOGIFACE_LOG(debug, "Expanded buffer sizes: pos=" + std::to_string(expanded_position_size) +
                 " attrib=" + std::to_string(expanded_attribute_size) +
                 " idx=" + std::to_string(expanded_index_size));

    for (auto& frame : frames_) {
        frame.instance_buffer = VulkanEngine::GpuResources::GpuBuffer::Create(
            backend, instance_data_size,
            vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

        frame.indirect_buffer = VulkanEngine::GpuResources::GpuBuffer::Create(
            backend, indirect_data_size,
            vk::BufferUsageFlagBits::eIndirectBuffer | vk::BufferUsageFlagBits::eTransferDst,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

        frame.depth_indirect_buffer = VulkanEngine::GpuResources::GpuBuffer::Create(
            backend, indirect_data_size,
            vk::BufferUsageFlagBits::eIndirectBuffer | vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
            vk::MemoryPropertyFlagBits::eDeviceLocal);

        frame.mesh_data_buffer = VulkanEngine::GpuResources::GpuBuffer::Create(
            backend, mesh_data_size,
            vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

        frame.expanded_position_buffer = VulkanEngine::GpuResources::GpuBuffer::Create(
            backend, expanded_position_size,
            vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
            vk::MemoryPropertyFlagBits::eDeviceLocal);

        frame.expanded_attribute_buffer = VulkanEngine::GpuResources::GpuBuffer::Create(
            backend, expanded_attribute_size,
            vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
            vk::MemoryPropertyFlagBits::eDeviceLocal);

        frame.expanded_index_buffer = VulkanEngine::GpuResources::GpuBuffer::Create(
            backend, expanded_index_size,
            vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eTransferDst,
            vk::MemoryPropertyFlagBits::eDeviceLocal);

        frame.expand_counter_buffer = VulkanEngine::GpuResources::GpuBuffer::Create(
            backend, counter_size,
            vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

        frame.camera_buffer = VulkanEngine::GpuResources::GpuBuffer::Create(
            backend, camera_ubo_size,
            vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eTransferDst,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

        // Instance descriptor set (used at set 0 in compute, set 1 in main pass)
        frame.instance_descriptor_set = instance_pool_->Allocate(*instance_data_layout_);
        if (frame.instance_descriptor_set.IsValid()) {
            frame.instance_descriptor_set.UpdateBinding(0, frame.instance_buffer,
                                                         vk::DescriptorType::eStorageBuffer);
        }

        // Expanded descriptor set (used at set 1 in compute, set 0 in depth)
        frame.expanded_descriptor_set = expanded_pool_->Allocate(*expanded_data_layout_);
        if (frame.expanded_descriptor_set.IsValid()) {
            frame.expanded_descriptor_set.UpdateBinding(0, frame.expanded_position_buffer,
                                                         vk::DescriptorType::eStorageBuffer);
            frame.expanded_descriptor_set.UpdateBinding(1, frame.expanded_attribute_buffer,
                                                         vk::DescriptorType::eStorageBuffer);
            frame.expanded_descriptor_set.UpdateBinding(2, frame.expanded_index_buffer,
                                                         vk::DescriptorType::eStorageBuffer);
            frame.expanded_descriptor_set.UpdateBinding(3, frame.depth_indirect_buffer,
                                                         vk::DescriptorType::eStorageBuffer);
            frame.expanded_descriptor_set.UpdateBinding(4, frame.expand_counter_buffer,
                                                         vk::DescriptorType::eStorageBuffer);
        }

        // Camera descriptor set (used at set 2 in compute, set 1 in depth)
        frame.camera_descriptor_set = camera_pool_->Allocate(*camera_data_layout_);
        if (frame.camera_descriptor_set.IsValid()) {
            frame.camera_descriptor_set.UpdateBinding(0, frame.camera_buffer,
                                                       vk::DescriptorType::eUniformBuffer);
            frame.camera_descriptor_set.UpdateBinding(1, frame.mesh_data_buffer,
                                                       vk::DescriptorType::eStorageBuffer);
            // Binding 2 (raw vertex buffer) updated per-frame in PrepareCompute
        }
    }

    LOGIFACE_LOG(info, "SceneRenderer initialized with GPU-driven expand + depth pipeline");
    return true;
}

bool SceneRenderer::CreateExpandPipeline(VulkanEngine::Runtime::IVulkanBootstrapBackend& backend) {
    const auto& device = backend.GetDevice();

    // Pipeline layout: sets [instance, expanded, camera] at indices 0, 1, 2
    std::array<vk::DescriptorSetLayout, 3> set_layouts = {
        *instance_data_layout_,
        *expanded_data_layout_,
        *camera_data_layout_
    };

    constexpr vk::PushConstantRange push_range(vk::ShaderStageFlagBits::eCompute, 0, sizeof(ExpandPushConstants));

    vk::PipelineLayoutCreateInfo pipeline_layout_info{};
    pipeline_layout_info.setLayoutCount = static_cast<uint32_t>(set_layouts.size());
    pipeline_layout_info.pSetLayouts = set_layouts.data();
    pipeline_layout_info.pushConstantRangeCount = 1;
    pipeline_layout_info.pPushConstantRanges = &push_range;
    compute_pipeline_layout_ = std::make_unique<vk::raii::PipelineLayout>(device, pipeline_layout_info);

    const std::filesystem::path shader_dir = SHADER_DIR;
    auto comp_spv = VulkanEngine::ShaderLoader::ShaderLoader::LoadSpirv(
        shader_dir / "expand.comp.spv");
    if (comp_spv.empty()) {
        LOGIFACE_LOG(error, "Failed to load expand compute shader");
        return false;
    }

    const vk::ShaderModuleCreateInfo module_info({}, comp_spv.size() * sizeof(uint32_t), comp_spv.data());
    const vk::raii::ShaderModule comp_module(device, module_info);

    const vk::PipelineShaderStageCreateInfo stage_info({}, vk::ShaderStageFlagBits::eCompute, *comp_module, "main");

    vk::ComputePipelineCreateInfo compute_info{};
    compute_info.stage = stage_info;
    compute_info.layout = *compute_pipeline_layout_;
    compute_pipeline_ = std::make_unique<vk::raii::Pipeline>(device, nullptr, compute_info);

    LOGIFACE_LOG(info, "Expand compute pipeline created");
    return true;
}

bool SceneRenderer::CreateDepthPipeline(VulkanEngine::Runtime::IVulkanBootstrapBackend& backend) {
    const auto& device = backend.GetDevice();

    // Pipeline layout: sets [expanded, camera] at indices 0, 1
    std::array<vk::DescriptorSetLayout, 2> set_layouts = {
        *expanded_data_layout_,
        *camera_data_layout_
    };

    vk::PipelineLayoutCreateInfo layout_info{};
    layout_info.setLayoutCount = static_cast<uint32_t>(set_layouts.size());
    layout_info.pSetLayouts = set_layouts.data();
    depth_pipeline_layout_ = std::make_unique<vk::raii::PipelineLayout>(device, layout_info);

    const std::filesystem::path shader_dir = SHADER_DIR;
    auto vert_spv = VulkanEngine::ShaderLoader::ShaderLoader::LoadSpirv(
        shader_dir / "depth_world.vert.spv");
    auto frag_spv = VulkanEngine::ShaderLoader::ShaderLoader::LoadSpirv(
        shader_dir / "depth_prepass.frag.spv");
    if (vert_spv.empty() || frag_spv.empty()) {
        LOGIFACE_LOG(error, "Failed to load depth pre-pass shaders");
        return false;
    }

    const vk::ShaderModuleCreateInfo vert_info({}, vert_spv.size() * sizeof(uint32_t), vert_spv.data());
    const vk::raii::ShaderModule vert_module(device, vert_info);
    const vk::ShaderModuleCreateInfo frag_info({}, frag_spv.size() * sizeof(uint32_t), frag_spv.data());
    const vk::raii::ShaderModule frag_module(device, frag_info);

    std::array<vk::PipelineShaderStageCreateInfo, 2> stages = {
        vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eVertex, *vert_module, "main"),
        vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eFragment, *frag_module, "main")
    };

    constexpr vk::PipelineVertexInputStateCreateInfo vertex_input({}, 0, nullptr, 0, nullptr);
    constexpr vk::PipelineInputAssemblyStateCreateInfo input_assembly({}, vk::PrimitiveTopology::eTriangleList);
    constexpr vk::PipelineViewportStateCreateInfo viewport_state({}, 1, nullptr, 1, nullptr);
    constexpr vk::PipelineRasterizationStateCreateInfo rasterization({}, false, false,
        vk::PolygonMode::eFill, vk::CullModeFlagBits::eFront, vk::FrontFace::eClockwise,
        false, 0, 0, 0, 1.0f);
    constexpr vk::PipelineMultisampleStateCreateInfo multisample({}, vk::SampleCountFlagBits::e1);
    const vk::PipelineDepthStencilStateCreateInfo depth_stencil({}, true, true, vk::CompareOp::eLess);

    constexpr vk::PipelineColorBlendAttachmentState color_blend_attachment;
    const vk::PipelineColorBlendStateCreateInfo color_blend({}, false, vk::LogicOp::eCopy, color_blend_attachment);

    std::array<vk::DynamicState, 2> dynamic_states = {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
    const vk::PipelineDynamicStateCreateInfo dynamic_state({}, dynamic_states);

    vk::PipelineRenderingCreateInfo rendering_info{};
    rendering_info.colorAttachmentCount = 0;
    rendering_info.pColorAttachmentFormats = nullptr;
    rendering_info.depthAttachmentFormat = backend.GetDepthFormat();

    vk::GraphicsPipelineCreateInfo pipeline_info({}, stages, &vertex_input, &input_assembly, nullptr, &viewport_state,
                                                  &rasterization, &multisample, &depth_stencil, &color_blend,
                                                  &dynamic_state, *depth_pipeline_layout_, nullptr, 0, {}, 0);
    pipeline_info.setPNext(&rendering_info);
    depth_pipeline_ = std::make_unique<vk::raii::Pipeline>(device, nullptr, pipeline_info);

    LOGIFACE_LOG(info, "Depth pre-pass pipeline created");
    return true;
}

void SceneRenderer::Shutdown() {
    if (backend_) {
        try { backend_->GetDevice().waitIdle(); }
        catch (const std::exception& err) {
            LOGIFACE_LOG(error, "Error during SceneRenderer shutdown: " + std::string(err.what()));
        }
    }

    for (auto& frame : frames_) {
        frame.camera_descriptor_set.~GpuDescriptorSet();
        new (&frame.camera_descriptor_set) VulkanEngine::GpuResources::GpuDescriptorSet();
        frame.expanded_descriptor_set.~GpuDescriptorSet();
        new (&frame.expanded_descriptor_set) VulkanEngine::GpuResources::GpuDescriptorSet();
        frame.instance_descriptor_set.~GpuDescriptorSet();
        new (&frame.instance_descriptor_set) VulkanEngine::GpuResources::GpuDescriptorSet();
    }

    camera_pool_.reset();
    expanded_pool_.reset();
    instance_pool_.reset();

    depth_pipeline_.reset();
    depth_pipeline_layout_.reset();
    compute_pipeline_.reset();
    compute_pipeline_layout_.reset();
    camera_data_layout_.reset();
    expanded_data_layout_.reset();
    instance_data_layout_.reset();

    for (auto& frame : frames_) {
        frame.camera_buffer = VulkanEngine::GpuResources::GpuBuffer{};
        frame.expand_counter_buffer = VulkanEngine::GpuResources::GpuBuffer{};
        frame.expanded_index_buffer = VulkanEngine::GpuResources::GpuBuffer{};
        frame.expanded_attribute_buffer = VulkanEngine::GpuResources::GpuBuffer{};
        frame.expanded_position_buffer = VulkanEngine::GpuResources::GpuBuffer{};
        frame.mesh_data_buffer = VulkanEngine::GpuResources::GpuBuffer{};
        frame.depth_indirect_buffer = VulkanEngine::GpuResources::GpuBuffer{};
        frame.indirect_buffer = VulkanEngine::GpuResources::GpuBuffer{};
        frame.instance_buffer = VulkanEngine::GpuResources::GpuBuffer{};
    }

    backend_ = nullptr;
}

vk::DescriptorSetLayout* SceneRenderer::GetInstanceDataLayout() const {
    return instance_data_layout_ ? const_cast<vk::DescriptorSetLayout*>(&**instance_data_layout_) : nullptr;
}

vk::DescriptorSetLayout* SceneRenderer::GetExpandedDataLayout() const {
    return expanded_data_layout_ ? const_cast<vk::DescriptorSetLayout*>(&**expanded_data_layout_) : nullptr;
}

vk::DescriptorSetLayout* SceneRenderer::GetCameraDataLayout() const {
    return camera_data_layout_ ? const_cast<vk::DescriptorSetLayout*>(&**camera_data_layout_) : nullptr;
}

void SceneRenderer::DispatchExpand(vk::CommandBuffer cmd, uint32_t entity_count, uint32_t frame_index) {
    const uint32_t frame = frame_index % FRAMES_IN_FLIGHT;
    auto& frame_res = frames_[frame];

    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *compute_pipeline_);

    std::array<vk::DescriptorSet, 3> desc_sets = {
        frame_res.instance_descriptor_set.GetHandle(),
        frame_res.expanded_descriptor_set.GetHandle(),
        frame_res.camera_descriptor_set.GetHandle()
    };
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *compute_pipeline_layout_, 0,
                           desc_sets, {});

    const ExpandPushConstants push{entity_count};
    cmd.pushConstants(*compute_pipeline_layout_, vk::ShaderStageFlagBits::eCompute, 0,
                      sizeof(ExpandPushConstants), &push);

    LOGIFACE_LOG(trace, "DispatchExpand: " + std::to_string(entity_count) + " entities, " +
                 std::to_string((entity_count + 63) / 64) + " work groups");
    cmd.dispatch((entity_count + 63) / 64, 1, 1);

    // Barriers: expand compute writes -> depth/main reads
    std::array<vk::BufferMemoryBarrier, 4> barriers{};
    barriers[0].srcAccessMask = vk::AccessFlagBits::eShaderWrite;
    barriers[0].dstAccessMask = vk::AccessFlagBits::eShaderRead;
    barriers[0].buffer = *frame_res.expanded_position_buffer.GetBuffer();
    barriers[0].size = VK_WHOLE_SIZE;

    barriers[1].srcAccessMask = vk::AccessFlagBits::eShaderWrite;
    barriers[1].dstAccessMask = vk::AccessFlagBits::eShaderRead;
    barriers[1].buffer = *frame_res.expanded_attribute_buffer.GetBuffer();
    barriers[1].size = VK_WHOLE_SIZE;

    barriers[2].srcAccessMask = vk::AccessFlagBits::eShaderWrite;
    barriers[2].dstAccessMask = vk::AccessFlagBits::eIndexRead;
    barriers[2].buffer = *frame_res.expanded_index_buffer.GetBuffer();
    barriers[2].size = VK_WHOLE_SIZE;

    barriers[3].srcAccessMask = vk::AccessFlagBits::eShaderWrite;
    barriers[3].dstAccessMask = vk::AccessFlagBits::eIndirectCommandRead;
    barriers[3].buffer = *frame_res.depth_indirect_buffer.GetBuffer();
    barriers[3].size = VK_WHOLE_SIZE;

    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                        vk::PipelineStageFlagBits::eVertexShader | vk::PipelineStageFlagBits::eDrawIndirect,
                        {}, {}, barriers, {});
}

void SceneRenderer::PrepareCompute(vk::CommandBuffer cmd,
                                    VulkanEngine::ComponentRegistry& registry,
                                    const VulkanEngine::GpuResources::GpuBuffer& raw_vertex_buffer,
                                    const VulkanEngine::GpuResources::GpuBuffer& original_index_buffer,
                                    const glm::mat4& view_matrix,
                                    const glm::mat4& projection_matrix,
                                    uint32_t width,
                                    uint32_t height,
                                    uint32_t frame_index) {
    const uint32_t frame = frame_index % FRAMES_IN_FLIGHT;
    auto& frame_res = frames_[frame];
    current_ranges_.clear();

    std::vector<DrawEntity> entities;
    registry.ForEach<VulkanEngine::Components::Material>([&](VulkanEngine::Components::Material& material) {
        auto* owner = material.GetOwner();
        if (!owner) return;
        auto* transform = owner->GetComponent<VulkanEngine::Components::Transform>();
        auto* mesh = owner->GetComponent<VulkanEngine::Components::MeshReference>();
        if (!transform || !mesh) return;
        if (mesh->index_count == 0) return;
        entities.push_back(DrawEntity{transform, mesh, &material});
    });
    current_entity_count_ = static_cast<uint32_t>(entities.size());
    if (current_entity_count_ == 0) return;

    std::ranges::sort(entities,
        [](const DrawEntity& a, const DrawEntity& b) {
            return a.material->technique_id < b.material->technique_id;
        });

    {
        uint32_t start = 0;
        for (size_t i = 1; i <= entities.size(); ++i) {
            if (i == entities.size() || entities[i].material->technique_id != entities[start].material->technique_id) {
                current_ranges_.push_back({entities[start].material->technique_id, start, static_cast<uint32_t>(i - start)});
                start = static_cast<uint32_t>(i);
            }
        }
    }

    std::vector<InstanceData> instance_data(current_entity_count_);
    std::vector<MeshData> mesh_data(current_entity_count_);

    for (uint32_t i = 0; i < current_entity_count_; ++i) {
        constexpr float pi = std::numbers::pi_v<float>;
        const float radians = entities[i].transform->rotation_degrees_y * (pi / 180.0f);
        instance_data[i].model_matrix =
            glm::translate(glm::mat4(1.0f), entities[i].transform->position) *
            glm::rotate(glm::mat4(1.0f), radians, glm::vec3(0.0f, 1.0f, 0.0f)) *
            glm::scale(glm::mat4(1.0f), entities[i].transform->scale);
        instance_data[i].technique_id = entities[i].material->technique_id;

        mesh_data[i].index_count = entities[i].mesh->index_count;
        mesh_data[i].vertex_count = entities[i].mesh->vertex_count;
        mesh_data[i].first_index = entities[i].mesh->index_offset;
        mesh_data[i].vertex_offset = static_cast<int32_t>(entities[i].mesh->vertex_offset);
    }

    frame_res.instance_buffer.Upload(instance_data.data(), current_entity_count_ * sizeof(InstanceData));
    frame_res.mesh_data_buffer.Upload(mesh_data.data(), current_entity_count_ * sizeof(MeshData));

    // Generate main-pass indirect commands on CPU (uses original index buffer)
    {
        std::vector<VkDrawIndexedIndirectCommand> main_indirect(current_entity_count_);
        for (uint32_t i = 0; i < current_entity_count_; ++i) {
            main_indirect[i].indexCount = mesh_data[i].index_count;
            main_indirect[i].instanceCount = 1;
            main_indirect[i].firstIndex = mesh_data[i].first_index;
            main_indirect[i].vertexOffset = mesh_data[i].vertex_offset;
            main_indirect[i].firstInstance = i;
        }
        frame_res.indirect_buffer.Upload(main_indirect.data(),
                                         current_entity_count_ * sizeof(VkDrawIndexedIndirectCommand));
    }

    // Camera UBO
    CameraUBO camera_ubo{};
    camera_ubo.view_proj = projection_matrix * view_matrix;
    const glm::mat4& vp = camera_ubo.view_proj;
    // Gribb-Hartmann frustum plane extraction
    camera_ubo.frustum_planes[0] = glm::vec4(vp[0][3] + vp[0][0], vp[1][3] + vp[1][0], vp[2][3] + vp[2][0], vp[3][3] + vp[3][0]);
    camera_ubo.frustum_planes[1] = glm::vec4(vp[0][3] - vp[0][0], vp[1][3] - vp[1][0], vp[2][3] - vp[2][0], vp[3][3] - vp[3][0]);
    camera_ubo.frustum_planes[2] = glm::vec4(vp[0][3] - vp[0][1], vp[1][3] - vp[1][1], vp[2][3] - vp[2][1], vp[3][3] - vp[3][1]);
    camera_ubo.frustum_planes[3] = glm::vec4(vp[0][3] + vp[0][1], vp[1][3] + vp[1][1], vp[2][3] + vp[2][1], vp[3][3] + vp[3][1]);
    camera_ubo.frustum_planes[4] = glm::vec4(vp[0][3] + vp[0][2], vp[1][3] + vp[1][2], vp[2][3] + vp[2][2], vp[3][3] + vp[3][2]);
    camera_ubo.frustum_planes[5] = glm::vec4(vp[0][3] - vp[0][2], vp[1][3] - vp[1][2], vp[2][3] - vp[2][2], vp[3][3] - vp[3][2]);
    frame_res.camera_buffer.Upload(&camera_ubo, sizeof(CameraUBO));

    // Reset expand counter
    const uint32_t zero = 0;
    frame_res.expand_counter_buffer.Upload(&zero, sizeof(uint32_t));

    // Update expanded descriptor set binding 5 with original index buffer
    if (original_index_buffer.GetSize() > 0) {
        frame_res.expanded_descriptor_set.UpdateBinding(5, original_index_buffer,
                                                         vk::DescriptorType::eStorageBuffer,
                                                         original_index_buffer.GetSize(), 0);
    }

    // Update camera descriptor set binding 2 with raw vertex buffer
    if (raw_vertex_buffer.GetSize() > 0) {
        frame_res.camera_descriptor_set.UpdateBinding(2, raw_vertex_buffer,
                                                       vk::DescriptorType::eStorageBuffer,
                                                       raw_vertex_buffer.GetSize(), 0);
    } else {
        LOGIFACE_LOG(warn, "No raw vertex buffer available for expand shader");
    }

    DispatchExpand(cmd, current_entity_count_, frame_index);
}

void SceneRenderer::DepthPrepass(vk::CommandBuffer cmd,
                                  uint32_t width,
                                  uint32_t height,
                                  uint32_t frame_index) {
    if (current_entity_count_ == 0) return;

    const uint32_t frame = frame_index % FRAMES_IN_FLIGHT;
    auto& frame_res = frames_[frame];

    cmd.setViewport(0, vk::Viewport(0.0f, static_cast<float>(height),
                                     static_cast<float>(width), -static_cast<float>(height), 0.0f, 1.0f));
    cmd.setScissor(0, vk::Rect2D({0, 0}, {width, height}));

    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *depth_pipeline_);
    cmd.bindIndexBuffer(*frame_res.expanded_index_buffer.GetBuffer(), 0, vk::IndexType::eUint32);

    std::array<vk::DescriptorSet, 2> depth_sets = {
        frame_res.expanded_descriptor_set.GetHandle(),
        frame_res.camera_descriptor_set.GetHandle()
    };
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *depth_pipeline_layout_, 0,
                           depth_sets, {});

    cmd.drawIndexedIndirect(*frame_res.depth_indirect_buffer.GetBuffer(),
                            0, current_entity_count_, sizeof(VkDrawIndexedIndirectCommand));
}

void SceneRenderer::Render(vk::CommandBuffer cmd,
                            VulkanEngine::ComponentRegistry& /*registry*/,
                            const VulkanEngine::GpuResources::GpuBuffer& vertex_buffer,
                            const VulkanEngine::GpuResources::GpuBuffer& index_buffer,
                            VulkanEngine::TechniqueManager::TechniqueManager& technique_mgr,
                            VulkanEngine::BindlessManager::BindlessManager& bindless_mgr,
                            const glm::mat4& projection_matrix,
                            const glm::mat4& view_matrix,
                            uint32_t width,
                            uint32_t height,
                            uint32_t frame_index) {
    if (width == 0U || height == 0U || current_entity_count_ == 0) {
        LOGIFACE_LOG(trace, "Render: skip (no entities)");
        return;
    }
    if (current_ranges_.empty()) {
        LOGIFACE_LOG(trace, "Render: skip (no technique ranges)");
        return;
    }

    const uint32_t frame = frame_index % FRAMES_IN_FLIGHT;
    auto& frame_res = frames_[frame];

    cmd.setViewport(0, vk::Viewport(0.0f, static_cast<float>(height),
                                     static_cast<float>(width), -static_cast<float>(height), 0.0f, 1.0f));
    cmd.setScissor(0, vk::Rect2D({0, 0}, {width, height}));

    const vk::Buffer vb_handle = *vertex_buffer.GetBuffer();
    const vk::Buffer ib_handle = *index_buffer.GetBuffer();
    cmd.bindVertexBuffers(0, {vb_handle}, {0});
    cmd.bindIndexBuffer(ib_handle, 0, vk::IndexType::eUint32);

    const glm::mat4 view_proj = projection_matrix * view_matrix;
    struct PushConstants { glm::mat4 viewProj; };
    const PushConstants constants{view_proj};

    for (const auto& range : current_ranges_) {
        auto* pipeline_mgr = technique_mgr.GetPipelineManager(range.technique_id);
        if (!pipeline_mgr) continue;

        auto* pipeline = pipeline_mgr->GetPipeline();
        auto* layout = pipeline_mgr->GetPipelineLayout();
        if (!pipeline || !layout) continue;

        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline);
        cmd.pushConstants(*layout, vk::ShaderStageFlagBits::eVertex, 0, sizeof(PushConstants), &constants);

        if (bindless_mgr.IsValid()) {
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *layout, 0,
                                   {bindless_mgr.GetDescriptorSet()}, {});
        }
        if (frame_res.instance_descriptor_set.GetHandle()) {
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *layout, 1,
                                   {frame_res.instance_descriptor_set.GetHandle()}, {});
        }

        const vk::DeviceSize cmd_offset = range.first_entity * sizeof(VkDrawIndexedIndirectCommand);
        cmd.drawIndexedIndirect(*frame_res.indirect_buffer.GetBuffer(),
                                cmd_offset, range.entity_count, sizeof(VkDrawIndexedIndirectCommand));
    }
}

} // namespace VulkanEngine::SceneRenderer
