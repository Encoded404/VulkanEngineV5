module;

#include <glm/glm.hpp> //NOLINT(misc-include-cleaner)

export module VulkanEngine.Renderer;

import std;
import std.compat;

import vulkan_hpp;

export import VulkanBackend.Runtime.VulkanBootstrap;
export import VulkanBackend.Component;
export import VulkanEngine.RenderPipeline;
export import VulkanEngine.StandardMeshPipeline;
export import VulkanEngine.SceneRenderer;
export import VulkanEngine.TechniqueManager;
export import VulkanEngine.BindlessManager;
export import VulkanEngine.Components.Camera;
export import VulkanEngine.GpuResources;
export import VulkanEngine.ImGui;

export namespace VulkanEngine::Renderer {

struct RendererConfig {
    bool enable_imgui = true;
    glm::vec4 clear_color{0.1f, 0.1f, 0.1f, 1.0f};
    vk::ClearDepthStencilValue clear_depth_stencil{1.0f, 0};
};

class Renderer {
public:
    Renderer() = default;
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    bool Initialize(VulkanEngine::Runtime::VulkanBootstrap& bootstrap,
                    const RendererConfig& config);

    void Shutdown();

    void RenderFrame(VulkanEngine::Runtime::VulkanBootstrap& bootstrap,
                     VulkanEngine::ComponentRegistry& registry,
                     const VulkanEngine::Components::Camera& camera,
                     VulkanEngine::TechniqueManager::TechniqueManager& technique_mgr,
                     VulkanEngine::BindlessManager::BindlessManager& bindless_mgr,
                     VulkanEngine::SceneRenderer::SceneRenderer& scene_renderer,
                     VulkanEngine::ImGui::ImGuiSystem* imgui,
                     std::uint32_t image_index);

private:
    VulkanEngine::Runtime::VulkanBootstrap* bootstrap_ = nullptr;
    std::unique_ptr<VulkanEngine::RenderPipeline::RenderPipeline> pipeline_{};

    VulkanEngine::ComponentRegistry* current_registry_ = nullptr;
    const VulkanEngine::Components::Camera* current_camera_ = nullptr;
    VulkanEngine::TechniqueManager::TechniqueManager* current_technique_mgr_ = nullptr;
    VulkanEngine::BindlessManager::BindlessManager* current_bindless_mgr_ = nullptr;
    VulkanEngine::SceneRenderer::SceneRenderer* current_scene_renderer_ = nullptr;
    VulkanEngine::ImGui::ImGuiSystem* current_imgui_ = nullptr;
    vk::ClearDepthStencilValue clear_depth_stencil_{1.0f, 0};
    std::uint32_t current_width_ = 0;
    std::uint32_t current_height_ = 0;
    std::uint32_t current_image_index_ = 0;
    glm::mat4 current_view_proj_{1.0f};
    std::uint32_t frame_counter_ = 0;
    std::uint32_t last_swapchain_image_count_ = 0;


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

} // namespace VulkanEngine::Renderer
