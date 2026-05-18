module;

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include <glm/glm.hpp>
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_raii.hpp>

export module VulkanEngine.DefaultRenderer;

export import VulkanBackend.Runtime.VulkanBootstrap;
export import VulkanBackend.Component;
export import VulkanEngine.RenderPipeline;
export import VulkanEngine.StandardMeshPipeline;
export import VulkanEngine.SceneRenderer;
export import VulkanEngine.TechniqueManager;
export import VulkanEngine.BindlessManager;
export import VulkanEngine.Components.Camera;
export import VulkanEngine.GpuResources;
export import VulkanEngine.ImGuiSystem;

export namespace VulkanEngine::DefaultRenderer {

struct DefaultRendererConfig {
    bool enable_imgui = true;
    glm::vec4 clear_color{0.1f, 0.1f, 0.1f, 1.0f};
    vk::ClearDepthStencilValue clear_depth_stencil{1.0f, 0};
};

class DefaultRenderer {
public:
    DefaultRenderer() = default;
    ~DefaultRenderer();

    DefaultRenderer(const DefaultRenderer&) = delete;
    DefaultRenderer& operator=(const DefaultRenderer&) = delete;

    bool Initialize(VulkanEngine::Runtime::VulkanBootstrap& bootstrap,
                    const DefaultRendererConfig& config);

    void Shutdown();

    void RenderFrame(VulkanEngine::Runtime::VulkanBootstrap& bootstrap,
                     VulkanEngine::ComponentRegistry& registry,
                     const VulkanEngine::Components::Camera& camera,
                     const VulkanEngine::GpuResources::GpuBuffer& vertex_buffer,
                     const VulkanEngine::GpuResources::GpuBuffer& index_buffer,
                     VulkanEngine::TechniqueManager::TechniqueManager& technique_mgr,
                     VulkanEngine::BindlessManager::BindlessManager& bindless_mgr,
                     VulkanEngine::SceneRenderer::SceneRenderer& scene_renderer,
                     VulkanEngine::ImGuiSystem::ImGuiSystem* imgui,
                     uint32_t image_index);

private:
    VulkanEngine::Runtime::VulkanBootstrap* bootstrap_ = nullptr;
    std::unique_ptr<VulkanEngine::RenderPipeline::RenderPipeline> pipeline_{};

    VulkanEngine::ComponentRegistry* current_registry_ = nullptr;
    const VulkanEngine::Components::Camera* current_camera_ = nullptr;
    const VulkanEngine::GpuResources::GpuBuffer* current_vertex_buffer_ = nullptr;
    const VulkanEngine::GpuResources::GpuBuffer* current_index_buffer_ = nullptr;
    VulkanEngine::TechniqueManager::TechniqueManager* current_technique_mgr_ = nullptr;
    VulkanEngine::BindlessManager::BindlessManager* current_bindless_mgr_ = nullptr;
    VulkanEngine::SceneRenderer::SceneRenderer* current_scene_renderer_ = nullptr;
    VulkanEngine::ImGuiSystem::ImGuiSystem* current_imgui_ = nullptr;
    uint32_t current_width_ = 0;
    uint32_t current_height_ = 0;
    uint32_t current_image_index_ = 0;
    uint32_t frame_counter_ = 0;

    static constexpr vk::QueryPipelineStatisticFlags GPU_STATS_FLAGS =
        vk::QueryPipelineStatisticFlagBits::eInputAssemblyVertices |
        vk::QueryPipelineStatisticFlagBits::eInputAssemblyPrimitives |
        vk::QueryPipelineStatisticFlagBits::eVertexShaderInvocations |
        vk::QueryPipelineStatisticFlagBits::eClippingInvocations |
        vk::QueryPipelineStatisticFlagBits::eClippingPrimitives |
        vk::QueryPipelineStatisticFlagBits::eFragmentShaderInvocations |
        vk::QueryPipelineStatisticFlagBits::eComputeShaderInvocations;
    std::unique_ptr<vk::raii::QueryPool> gpu_stats_pool_{};
};

} // namespace VulkanEngine::DefaultRenderer
