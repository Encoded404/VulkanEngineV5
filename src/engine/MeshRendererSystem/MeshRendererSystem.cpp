module;

#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_RADIANS
#include <glm/glm.hpp> // NOLINT(misc-include-cleaner)
#include <glm/gtc/matrix_transform.hpp> // NOLINT(misc-include-cleaner)
#include <vulkan/vulkan_raii.hpp>

module VulkanEngine.MeshRendererSystem;

import VulkanBackend.Component;
import VulkanEngine.Components.Transform;
import VulkanEngine.Components.MeshRenderer;
import VulkanEngine.GpuResources;

namespace VulkanEngine::MeshRendererSystem {

namespace {

struct PushConstants {
    glm::mat4 mvp{};
};

[[nodiscard]] PushConstants BuildPushConstants(const VulkanEngine::Components::Transform& transform, float aspect) {
    constexpr float pi = std::numbers::pi_v<float>;
    const float radians = transform.rotation_degrees_y * (pi / 180.0f);

    const glm::mat4 model = glm::translate(glm::mat4(1.0f), transform.position)
                          * glm::rotate(glm::mat4(1.0f), radians, glm::vec3(0.0f, 1.0f, 0.0f))
                          * glm::scale(glm::mat4(1.0f), transform.scale);
    const glm::mat4 view = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -3.0f));
    const glm::mat4 proj = glm::perspective(60.0f * (pi / 180.0f), aspect, 0.1f, 100.0f);

    PushConstants constants{};
    constants.mvp = proj * view * model;
    return constants;
}

} // namespace

void MeshRendererSystem::RecordAllMeshDraws(vk::CommandBuffer cmd,
                                            VulkanEngine::ComponentRegistry& registry,
                                            const MeshRenderObject& render_object,
                                            uint32_t width,
                                            uint32_t height) {
    if (width == 0U || height == 0U) return;
    if (render_object.index_count == 0) return;
    if (!render_object.pipeline || !render_object.pipeline_layout ||
        !render_object.vertex_buffer || !render_object.index_buffer) return;

    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *render_object.pipeline);
    cmd.setViewport(0, vk::Viewport(0.0f, static_cast<float>(height), static_cast<float>(width), -static_cast<float>(height), 0.0f, 1.0f));
    cmd.setScissor(0, vk::Rect2D({0, 0}, {width, height}));
    cmd.bindVertexBuffers(0, {*render_object.vertex_buffer->GetBuffer()}, {0});
    cmd.bindIndexBuffer(*render_object.index_buffer->GetBuffer(), 0, vk::IndexType::eUint32);

    if (render_object.descriptor_set) {
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *render_object.pipeline_layout, 0,
                               {render_object.descriptor_set}, {});
    }

    const float aspect = static_cast<float>(width) / static_cast<float>(height);

    registry.ForEach<VulkanEngine::Components::MeshRenderer>([&](VulkanEngine::Components::MeshRenderer& mesh_renderer) {
        if (!mesh_renderer.visible) {
            return;
        }

        auto* transform = mesh_renderer.GetTransform();
        if (transform == nullptr) {
            return;
        }

        const PushConstants push_constants = BuildPushConstants(*transform, aspect);
        cmd.pushConstants(*render_object.pipeline_layout,
                          vk::ShaderStageFlagBits::eVertex,
                          0,
                          sizeof(PushConstants),
                          &push_constants);
        cmd.drawIndexed(render_object.index_count, 1, 0, 0, 0);
    });
}

} // namespace VulkanEngine::MeshRendererSystem
