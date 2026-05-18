module;

#include <algorithm>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <numbers>
#include <cstring>
#include <array>
#include <filesystem>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_RADIANS
#include <glm/glm.hpp> //NOLINT(misc-include-cleaner)
#include <glm/gtc/matrix_transform.hpp> //NOLINT(misc-include-cleaner)
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

struct PushConstants {
    glm::mat4 viewProj{};
};

struct DrawEntity {
    const VulkanEngine::Components::Transform* transform = nullptr;
    const VulkanEngine::Components::MeshReference* mesh = nullptr;
    const VulkanEngine::Components::Material* material = nullptr;
};

} // namespace

SceneRenderer::~SceneRenderer() {
    Shutdown();
}

bool SceneRenderer::Initialize(VulkanEngine::Runtime::IVulkanBootstrapBackend& backend) {
    backend_ = &backend;
    const auto& device = backend.GetDevice();

    // ── Graphics: instance data descriptor set (set=1) ──
    {
        vk::DescriptorSetLayoutBinding instance_binding{};
        instance_binding.binding = 0;
        instance_binding.descriptorType = vk::DescriptorType::eStorageBuffer;
        instance_binding.descriptorCount = 1;
        instance_binding.stageFlags = vk::ShaderStageFlagBits::eVertex;

        vk::DescriptorSetLayoutCreateInfo layout_info{};
        layout_info.bindingCount = 1;
        layout_info.pBindings = &instance_binding;
        instance_data_layout_ = std::make_unique<vk::raii::DescriptorSetLayout>(device, layout_info);

        VulkanEngine::GpuResources::DescriptorPoolConfig pool_config{};
        pool_config.max_sets = FRAMES_IN_FLIGHT;
        pool_config.max_storage_buffers = FRAMES_IN_FLIGHT;
        gfx_pool_ = VulkanEngine::GpuResources::DescriptorPool::Create(backend, pool_config);
    }

    // ── Compute pipeline (generate_indirect.comp) ──
    if (!CreateComputePipeline(backend)) {
        return false;
    }

    // ── Per-frame resources ──
    constexpr uint64_t instance_data_size = MAX_ENTITIES * sizeof(InstanceData);
    constexpr uint64_t indirect_data_size = MAX_ENTITIES * sizeof(VkDrawIndexedIndirectCommand);
    constexpr uint64_t mesh_data_size = MAX_ENTITIES * sizeof(MeshData);

    for (auto &[instance_buffer, indirect_buffer, mesh_data_buffer, gfx_descriptor_set, compute_descriptor_set] : frames_) {
        // Instance data buffer (CPU→GPU, read by compute + vertex shader)
        instance_buffer = VulkanEngine::GpuResources::GpuBuffer::Create(
            backend, instance_data_size,
            vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

        // Indirect command buffer (GPU→GPU, written by compute, read by draw)
        indirect_buffer = VulkanEngine::GpuResources::GpuBuffer::Create(
            backend, indirect_data_size,
            vk::BufferUsageFlagBits::eIndirectBuffer | vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

        // Mesh data buffer (CPU→GPU, per-entity mesh info, read by compute)
        mesh_data_buffer = VulkanEngine::GpuResources::GpuBuffer::Create(
            backend, mesh_data_size,
            vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

        // Graphics descriptor set (set=1: instance data)
        gfx_descriptor_set = gfx_pool_->Allocate(*instance_data_layout_);
        if (gfx_descriptor_set.IsValid()) {
            gfx_descriptor_set.UpdateBinding(0, instance_buffer,
                                                         vk::DescriptorType::eStorageBuffer);
        }

        // Compute descriptor set (set=0: instance + mesh + indirect)
        compute_descriptor_set = compute_pool_->Allocate(*compute_layout_);
        if (compute_descriptor_set.IsValid()) {
            compute_descriptor_set.UpdateBinding(0, instance_buffer,
                                                             vk::DescriptorType::eStorageBuffer);
            compute_descriptor_set.UpdateBinding(1, mesh_data_buffer,
                                                             vk::DescriptorType::eStorageBuffer);
            compute_descriptor_set.UpdateBinding(2, indirect_buffer,
                                                             vk::DescriptorType::eStorageBuffer);
        }
    }

    LOGIFACE_LOG(debug, "SceneRenderer initialized with compute indirect generation");
    return true;
}

bool SceneRenderer::CreateComputePipeline(VulkanEngine::Runtime::IVulkanBootstrapBackend& backend) {
    const auto& device = backend.GetDevice();

    // Compute descriptor set layout
    std::array<vk::DescriptorSetLayoutBinding, 3> compute_bindings{};
    compute_bindings[0].binding = 0;
    compute_bindings[0].descriptorType = vk::DescriptorType::eStorageBuffer;
    compute_bindings[0].descriptorCount = 1;
    compute_bindings[0].stageFlags = vk::ShaderStageFlagBits::eCompute;

    compute_bindings[1].binding = 1;
    compute_bindings[1].descriptorType = vk::DescriptorType::eStorageBuffer;
    compute_bindings[1].descriptorCount = 1;
    compute_bindings[1].stageFlags = vk::ShaderStageFlagBits::eCompute;

    compute_bindings[2].binding = 2;
    compute_bindings[2].descriptorType = vk::DescriptorType::eStorageBuffer;
    compute_bindings[2].descriptorCount = 1;
    compute_bindings[2].stageFlags = vk::ShaderStageFlagBits::eCompute;

    vk::DescriptorSetLayoutCreateInfo compute_layout_info{};
    compute_layout_info.bindingCount = static_cast<uint32_t>(compute_bindings.size());
    compute_layout_info.pBindings = compute_bindings.data();
    compute_layout_ = std::make_unique<vk::raii::DescriptorSetLayout>(device, compute_layout_info);

    // Compute pipeline layout
    constexpr vk::PushConstantRange push_range(vk::ShaderStageFlagBits::eCompute, 0, sizeof(uint32_t));
    vk::PipelineLayoutCreateInfo pipeline_layout_info{};
    pipeline_layout_info.setLayoutCount = 1;
    pipeline_layout_info.pSetLayouts = &**compute_layout_;
    pipeline_layout_info.pushConstantRangeCount = 1;
    pipeline_layout_info.pPushConstantRanges = &push_range;
    compute_pipeline_layout_ = std::make_unique<vk::raii::PipelineLayout>(device, pipeline_layout_info);

    // Load compute shader SPIR-V
    const std::filesystem::path shader_dir = SHADER_DIR;
    auto comp_spv = VulkanEngine::ShaderLoader::ShaderLoader::LoadSpirv(
        shader_dir / "generate_indirect.comp.spv");
    LOGIFACE_LOG(debug, "Loading compute SPIR-V from: " + (shader_dir / "generate_indirect.comp.spv").string() +
                 " size=" + std::to_string(comp_spv.size()) + " words");
    if (comp_spv.empty()) {
        LOGIFACE_LOG(error, "Failed to load compute shader");
        return false;
    }

    const vk::ShaderModuleCreateInfo module_info({}, comp_spv.size() * sizeof(uint32_t), comp_spv.data());
    const vk::raii::ShaderModule comp_module(device, module_info);

    const vk::PipelineShaderStageCreateInfo stage_info({}, vk::ShaderStageFlagBits::eCompute, *comp_module, "main");

    vk::ComputePipelineCreateInfo compute_info{};
    compute_info.stage = stage_info;
    compute_info.layout = *compute_pipeline_layout_;
    compute_pipeline_ = std::make_unique<vk::raii::Pipeline>(device, nullptr, compute_info);

    // Compute descriptor pool
    VulkanEngine::GpuResources::DescriptorPoolConfig pool_config{};
    pool_config.max_sets = FRAMES_IN_FLIGHT;
    pool_config.max_storage_buffers = FRAMES_IN_FLIGHT * 3;
    compute_pool_ = VulkanEngine::GpuResources::DescriptorPool::Create(backend, pool_config);

    LOGIFACE_LOG(info, "Compute pipeline created for indirect command generation");
    return true;
}

void SceneRenderer::Shutdown() {
    if (backend_) {
        try {
            backend_->GetDevice().waitIdle();
        }
        catch (const std::exception& err) {
            LOGIFACE_LOG(error, "Error during SceneRenderer shutdown: " + std::string(err.what()));
        }
    }
    // Destroy GpuDescriptorSets first (they hold pool references)
    // GpuDescriptorSet::~GpuDescriptorSet() calls FreeDescriptorSet on the pool,
    // then releases the shared_ptr. The pool stays alive via compute_pool_/gfx_pool_.
    for (auto& frame : frames_) {
        frame.compute_descriptor_set.~GpuDescriptorSet();
        new (&frame.compute_descriptor_set) VulkanEngine::GpuResources::GpuDescriptorSet();
        frame.gfx_descriptor_set.~GpuDescriptorSet();
        new (&frame.gfx_descriptor_set) VulkanEngine::GpuResources::GpuDescriptorSet();
    }

    // Destroy pools (all descriptor sets within them are now freed by the dtors above)
    compute_pool_.reset();
    gfx_pool_.reset();

    // Destroy pipeline resources
    compute_pipeline_.reset();
    compute_pipeline_layout_.reset();
    compute_layout_.reset();
    instance_data_layout_.reset();

    // Clear frame buffers (GpuBuffer move assignment handles cleanup correctly)
    for (auto& frame : frames_) {
        frame.instance_buffer = VulkanEngine::GpuResources::GpuBuffer{};
        frame.indirect_buffer = VulkanEngine::GpuResources::GpuBuffer{};
        frame.mesh_data_buffer = VulkanEngine::GpuResources::GpuBuffer{};
    }

    backend_ = nullptr;
}

vk::DescriptorSetLayout* SceneRenderer::GetInstanceDataLayout() const {
    return instance_data_layout_ ? const_cast<vk::DescriptorSetLayout*>(&**instance_data_layout_) : nullptr;
}

void SceneRenderer::DispatchCompute(vk::CommandBuffer cmd, uint32_t entity_count, uint32_t frame_index) {
    const uint32_t frame = frame_index % FRAMES_IN_FLIGHT;
    auto& frame_res = frames_[frame];

    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *compute_pipeline_);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *compute_pipeline_layout_, 0,
                           {frame_res.compute_descriptor_set.GetHandle()}, {});
    cmd.pushConstants(*compute_pipeline_layout_, vk::ShaderStageFlagBits::eCompute, 0,
                      sizeof(uint32_t), &entity_count);
    LOGIFACE_LOG(trace, "DispatchCompute: " + std::to_string(entity_count) + " entities, " +
                 std::to_string((entity_count + 63) / 64) + " work groups");
    cmd.dispatch((entity_count + 63) / 64, 1, 1);

    // Barrier: compute shader writes → indirect read
    vk::BufferMemoryBarrier barrier{};
    barrier.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
    barrier.dstAccessMask = vk::AccessFlagBits::eIndirectCommandRead;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = *frame_res.indirect_buffer.GetBuffer();
    barrier.size = VK_WHOLE_SIZE;
    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                        vk::PipelineStageFlagBits::eDrawIndirect,
                        {}, {}, {barrier}, {});

    // Debug: read back indirect buffer and mesh data buffer to verify compute shader output
    {
        void* mapped = frame_res.indirect_buffer.Map(0, sizeof(VkDrawIndexedIndirectCommand));
        if (mapped) {
            auto* draw_cmd = static_cast<VkDrawIndexedIndirectCommand*>(mapped);
            LOGIFACE_LOG(trace, "INDIRECT DEBUG: indexCount=" + std::to_string(draw_cmd->indexCount) +
                         " instanceCount=" + std::to_string(draw_cmd->instanceCount) +
                         " firstIndex=" + std::to_string(draw_cmd->firstIndex) +
                         " vertexOffset=" + std::to_string(draw_cmd->vertexOffset) +
                         " firstInstance=" + std::to_string(draw_cmd->firstInstance));
            frame_res.indirect_buffer.Unmap();
        }
    }
    {
        void* mapped = frame_res.mesh_data_buffer.Map(0, sizeof(MeshData));
        if (mapped) {
            auto* md = static_cast<MeshData*>(mapped);
            LOGIFACE_LOG(trace, "MESH_DATA DEBUG: indexCount=" + std::to_string(md->index_count) +
                         " instanceCount=" + std::to_string(md->instance_count) +
                         " firstIndex=" + std::to_string(md->first_index) +
                         " vertexOffset=" + std::to_string(md->vertex_offset));
            frame_res.mesh_data_buffer.Unmap();
        }
    }
}

void SceneRenderer::PrepareCompute(vk::CommandBuffer cmd,
                                    VulkanEngine::ComponentRegistry& registry,
                                    uint32_t frame_index) {
    const uint32_t frame = frame_index % FRAMES_IN_FLIGHT;
    auto& frame_res = frames_[frame];
    current_ranges_.clear();

    // Debug: read previous frame's indirect buffer (already executed by GPU)
    if (frame_index > 0) {
        const uint32_t prev_frame = (frame_index - 1) % FRAMES_IN_FLIGHT;
        auto& prev_res = frames_[prev_frame];
        void* pmapped = prev_res.indirect_buffer.Map(0, sizeof(VkDrawIndexedIndirectCommand));
        if (pmapped) {
            auto* draw_cmd = static_cast<VkDrawIndexedIndirectCommand*>(pmapped);
            LOGIFACE_LOG(trace, "PREV_INDIRECT DEBUG: indexCount=" + std::to_string(draw_cmd->indexCount) +
                         " instanceCount=" + std::to_string(draw_cmd->instanceCount) +
                         " firstIndex=" + std::to_string(draw_cmd->firstIndex) +
                         " vertexOffset=" + std::to_string(draw_cmd->vertexOffset) +
                         " firstInstance=" + std::to_string(draw_cmd->firstInstance));
            prev_res.indirect_buffer.Unmap();
        }
    }

    // Collect all drawable entities
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
    LOGIFACE_LOG(trace, "PrepareCompute: " + std::to_string(current_entity_count_) + " entities collected");
    if (current_entity_count_ == 0) return;

    // Sort by technique_id
    std::ranges::sort(entities,
        [](const DrawEntity& a, const DrawEntity& b) {
            return a.material->technique_id < b.material->technique_id;
        });

    // Build technique ranges
    {
        uint32_t start = 0;
        for (size_t i = 1; i <= entities.size(); ++i) {
            if (i == entities.size() || entities[i].material->technique_id != entities[start].material->technique_id) {
                current_ranges_.push_back({entities[start].material->technique_id, start, static_cast<uint32_t>(i - start)});
                start = static_cast<uint32_t>(i);
            }
        }
    }

    // Instance data + mesh data
    std::vector<InstanceData> instance_data(current_entity_count_);
    std::vector<MeshData> mesh_data(current_entity_count_);

    for (uint32_t i = 0; i < current_entity_count_; ++i) {
        constexpr float pi = std::numbers::pi_v<float>;
        const float radians = entities[i].transform->rotation_degrees_y * (pi / 180.0f);
        instance_data[i].model_matrix =
            glm::translate(glm::mat4(1.0f), entities[i].transform->position) *
            glm::rotate(glm::mat4(1.0f), radians, glm::vec3(0.0f, 1.0f, 0.0f)) *
            glm::scale(glm::mat4(1.0f), entities[i].transform->scale);
        instance_data[i].material_id = entities[i].material->bindless_texture_slot;

        mesh_data[i].index_count = entities[i].mesh->index_count;
        mesh_data[i].instance_count = 1;
        mesh_data[i].first_index = entities[i].mesh->index_offset;
        mesh_data[i].vertex_offset = 0;
    }

    // Upload to GPU
    frame_res.instance_buffer.Upload(instance_data.data(), current_entity_count_ * sizeof(InstanceData));
    frame_res.mesh_data_buffer.Upload(mesh_data.data(), current_entity_count_ * sizeof(MeshData));

    // Dispatch compute shader to generate indirect commands
    DispatchCompute(cmd, current_entity_count_, frame_index);
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
    LOGIFACE_LOG(trace, "Render: " + std::to_string(current_ranges_.size()) + " techniques, " +
                 std::to_string(current_entity_count_) + " entities");

// if we are compiling for debug
#ifdef DEBUG
    // Debug: log camera/view-proj info
    {
        const glm::mat4 inv_view = glm::inverse(view_matrix);
        const glm::vec3 world_pos = glm::vec3(inv_view[3]);
        LOGIFACE_LOG(trace, "CAMERA DEBUG: world_pos=(" +
                     std::to_string(world_pos.x) + "," + std::to_string(world_pos.y) + "," + std::to_string(world_pos.z) +
                     ") proj[0][0]=" + std::to_string(projection_matrix[0][0]) +
                     " proj[1][1]=" + std::to_string(projection_matrix[1][1]) +
                     " proj[2][2]=" + std::to_string(projection_matrix[2][2]) +
                     " proj[3][2]=" + std::to_string(projection_matrix[3][2]) +
                     " width=" + std::to_string(width) + " height=" + std::to_string(height));

        // Log first entity's model matrix
        void* imapped = frame_res.instance_buffer.Map(0, sizeof(InstanceData));
        if (imapped) {
            auto* inst = static_cast<InstanceData*>(imapped);
            LOGIFACE_LOG(trace, "MODEL MATRIX DEBUG: col0=(" +
                         std::to_string(inst->model_matrix[0][0]) + "," + std::to_string(inst->model_matrix[1][0]) + "," + std::to_string(inst->model_matrix[2][0]) + "," + std::to_string(inst->model_matrix[3][0]) +
                         ") col1=(" +
                         std::to_string(inst->model_matrix[0][1]) + "," + std::to_string(inst->model_matrix[1][1]) + "," + std::to_string(inst->model_matrix[2][1]) + "," + std::to_string(inst->model_matrix[3][1]) +
                         ") col2=(" +
                         std::to_string(inst->model_matrix[0][2]) + "," + std::to_string(inst->model_matrix[1][2]) + "," + std::to_string(inst->model_matrix[2][2]) + "," + std::to_string(inst->model_matrix[3][2]) +
                         ") col3=(" +
                         std::to_string(inst->model_matrix[0][3]) + "," + std::to_string(inst->model_matrix[1][3]) + "," + std::to_string(inst->model_matrix[2][3]) + "," + std::to_string(inst->model_matrix[3][3]) + ")");
            frame_res.instance_buffer.Unmap();
        }

        // Debug: read vertex buffer AABB
        {
            auto& vb = const_cast<VulkanEngine::GpuResources::GpuBuffer&>(vertex_buffer);
            const uint64_t vb_size = vb.GetSize();
            constexpr uint32_t vb_stride = 36;
            const uint32_t vcount = static_cast<uint32_t>(vb_size / vb_stride);
            if (const void* vmapped = vb.Map(0, vb_size)) {
                float min_x = 1e30f, min_y = 1e30f, min_z = 1e30f;
                float max_x = -1e30f, max_y = -1e30f, max_z = -1e30f;
                struct Vert { float px, py, pz; };
                auto* verts = static_cast<const Vert*>(vmapped);
                for (uint32_t i = 0; i < std::min(vcount, 1966u); ++i) {
                    min_x = std::min(min_x, verts[i].px); max_x = std::max(max_x, verts[i].px);
                    min_y = std::min(min_y, verts[i].py); max_y = std::max(max_y, verts[i].py);
                    min_z = std::min(min_z, verts[i].pz); max_z = std::max(max_z, verts[i].pz);
                }
                LOGIFACE_LOG(trace, "VERTEX AABB: count=" + std::to_string(vcount) +
                             " x=[" + std::to_string(min_x) + "," + std::to_string(max_x) + "]" +
                             " y=[" + std::to_string(min_y) + "," + std::to_string(max_y) + "]" +
                             " z=[" + std::to_string(min_z) + "," + std::to_string(max_z) + "]");
                LOGIFACE_LOG(trace, "VERTEX SAMPLE: first=(" +
                             std::to_string(verts[0].px) + "," + std::to_string(verts[0].py) + "," + std::to_string(verts[0].pz) + ")" +
                             " last=(" +
                             std::to_string(verts[vcount-1].px) + "," + std::to_string(verts[vcount-1].py) + "," + std::to_string(verts[vcount-1].pz) + ")");
                vb.Unmap();
            }
        }

        // Debug: read index buffer range
        {
            auto& ib = const_cast<VulkanEngine::GpuResources::GpuBuffer&>(index_buffer);
            const uint64_t ib_size = ib.GetSize();
            const void* imapped2 = ib.Map(0, ib_size);
            if (imapped2) {
                uint32_t min_idx = 0xFFFFFFFF, max_idx = 0;
                auto* indices = static_cast<const uint32_t*>(imapped2);
                for (uint64_t i = 0; i < ib_size / sizeof(uint32_t); ++i) {
                    min_idx = std::min(min_idx, indices[i]);
                    max_idx = std::max(max_idx, indices[i]);
                }
                LOGIFACE_LOG(trace, "INDEX DEBUG: count=" + std::to_string(ib_size / sizeof(uint32_t)) +
                             " min=" + std::to_string(min_idx) + " max=" + std::to_string(max_idx) +
                             " indices[0]=" + std::to_string(indices[0]) +
                             " indices[1]=" + std::to_string(indices[1]) +
                             " indices[2]=" + std::to_string(indices[2]));
                ib.Unmap();
            }
        }
#endif
    }

    cmd.setViewport(0, vk::Viewport(0.0f, static_cast<float>(height),
                                     static_cast<float>(width), -static_cast<float>(height), 0.0f, 1.0f));
    cmd.setScissor(0, vk::Rect2D({0, 0}, {width, height}));

    const vk::Buffer vb_handle = *vertex_buffer.GetBuffer();
    const vk::Buffer ib_handle = *index_buffer.GetBuffer();
    cmd.bindVertexBuffers(0, {vb_handle}, {0});
    cmd.bindIndexBuffer(ib_handle, 0, vk::IndexType::eUint32);

    const glm::mat4 view_proj = projection_matrix * view_matrix;
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
        if (frame_res.gfx_descriptor_set.GetHandle()) {
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *layout, 1,
                                   {frame_res.gfx_descriptor_set.GetHandle()}, {});
        }

        const vk::DeviceSize cmd_offset = range.first_entity * sizeof(VkDrawIndexedIndirectCommand);
        LOGIFACE_LOG(trace, "  draw technique=" + std::to_string(range.technique_id) +
                     " entities=" + std::to_string(range.entity_count) +
                     " offset=" + std::to_string(cmd_offset));

        cmd.drawIndexedIndirect(*frame_res.indirect_buffer.GetBuffer(),
                                cmd_offset, range.entity_count, sizeof(VkDrawIndexedIndirectCommand));
    }
}

}
