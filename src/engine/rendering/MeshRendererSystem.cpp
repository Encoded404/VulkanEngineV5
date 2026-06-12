module;

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_RADIANS
#include <glm/glm.hpp> //NOLINT(misc-include-cleaner)
#include <glm/gtc/matrix_transform.hpp> //NOLINT(misc-include-cleaner)
#include <glm/gtc/quaternion.hpp> //NOLINT(misc-include-cleaner)

module VulkanEngine.MeshDrawRecorder;

import vulkan_hpp;

import VulkanBackend.Component;
import VulkanEngine.Components.Transform;
import VulkanEngine.Components.MeshRenderer;
import VulkanEngine.GpuResources;

namespace VulkanEngine::MeshDrawRecorder {

namespace {

struct PushConstants {
    glm::mat4 mvp{};
};

[[nodiscard]] PushConstants BuildPushConstants(const VulkanEngine::Components::Transform& transform,
                                                const glm::mat4& view,
                                                const glm::mat4& proj) {
    const auto pos = glm::vec3(transform.position);
    const auto rot = glm::quat(transform.rotation);
    const auto scl = glm::vec3(transform.scale);
    const glm::mat4 model = glm::translate(glm::mat4(1.0f), pos)
                          * glm::mat4_cast(rot)
                          * glm::scale(glm::mat4(1.0f), scl);

    PushConstants constants{};
    constants.mvp = proj * view * model;
    return constants;
}

} // namespace

void MeshDrawRecorder::RecordAllMeshDraws(vk::CommandBuffer cmd,
                                            VulkanEngine::ComponentRegistry& registry,
                                            const MeshRenderObject& render_object,
                                            const glm::mat4& view_matrix,
                                            const glm::mat4& projection_matrix,
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

    registry.ForEach<VulkanEngine::Components::MeshRenderer>([&](VulkanEngine::Components::MeshRenderer& mesh_renderer) {
        if (!mesh_renderer.visible) {
            return;
        }

        auto* transform = mesh_renderer.GetTransform();
        if (transform == nullptr) {
            return;
        }

        const PushConstants push_constants = BuildPushConstants(*transform, view_matrix, projection_matrix);
        cmd.pushConstants(*render_object.pipeline_layout,
                          vk::ShaderStageFlagBits::eVertex,
                          0,
                          sizeof(PushConstants),
                          &push_constants);
        cmd.drawIndexed(render_object.index_count, 1, 0, 0, 0);
    });
}

} // namespace VulkanEngine::MeshDrawRecorder
