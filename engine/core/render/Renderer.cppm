module;

#include <glm/glm.hpp> //NOLINT(misc-include-cleaner)

export module VulkanEngine.Renderer;

import std;
import std.compat;

import vulkan_hpp;

export import VulkanBackend.Vulkan.VulkanBootstrap;
export import VulkanEngine.ECS.ComponentRegistry;
export import VulkanEngine.RenderPipeline;
export import VulkanEngine.StandardMeshPipeline;
export import VulkanEngine.SceneRenderer;
export import VulkanEngine.TechniqueManager;
export import VulkanEngine.BindlessManager;
export import VulkanEngine.Components.Camera;
export import VulkanEngine.GpuResources;
export import VulkanEngine.ImGui;
import VulkanEngine.ShaderManager;
import VulkanEngine.PipelineFactory;
import VulkanEngine.Render.Passes.ExpandPass;
import VulkanEngine.Render.Passes.OccluderSelectPass;
import VulkanEngine.Render.Passes.OccluderPrePass;
import VulkanEngine.Render.Passes.DepthPrePass;
import VulkanEngine.Render.Passes.HiZPass;
import VulkanEngine.Render.Passes.PreCullPass;
import VulkanEngine.Render.Passes.OcclusionPass;
import VulkanEngine.Render.Passes.CollectPass;
import VulkanEngine.Render.Passes.MainPass;
import VulkanEngine.Render.Passes.ImGuiPass;

#ifdef VKENGINE_PHYSICAL_CAMERA
import VulkanEngine.PhysicalCameraSystem;
#endif

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

    bool Initialize(VulkanBackend::Vulkan::VulkanBootstrap& bootstrap,
                    const RendererConfig& config,
                    VulkanEngine::SceneRenderer::SceneRenderer& scene_renderer,
                    ShaderSystem::ShaderManager* shader_manager = nullptr,
                    ShaderSystem::PipelineFactory* pipeline_factory = nullptr);

    // Engine-standard set layouts (0-4) used to build custom-pass pipelines.
    void SetEngineDescriptorSetLayouts(std::array<vk::DescriptorSetLayout, 5> layouts);

    [[nodiscard]] VulkanEngine::RenderPipeline::RenderPipeline& GetRenderPipeline();
    [[nodiscard]] const VulkanEngine::RenderPipeline::RenderPipeline& GetRenderPipeline() const;

    void Shutdown();

    void RenderFrame(VulkanBackend::Vulkan::VulkanBootstrap& bootstrap,
                     VulkanEngine::ComponentRegistry& registry,
                     const VulkanEngine::Components::Camera& camera,
                     VulkanEngine::TechniqueManager::TechniqueManager& technique_mgr,
                     VulkanEngine::BindlessManager::BindlessManager& bindless_mgr,
                     VulkanEngine::SceneRenderer::SceneRenderer& scene_renderer,
                     VulkanEngine::ImGui::ImGuiSystem* imgui,
                     std::uint32_t image_index
#ifdef VKENGINE_PHYSICAL_CAMERA
                     , VulkanEngine::PhysicalCamera::PhysicalCameraSystem* physical_cameras = nullptr
#endif
                     );

private:
    VulkanBackend::Vulkan::VulkanBootstrap* bootstrap_ = nullptr;
    std::unique_ptr<VulkanEngine::RenderPipeline::RenderPipeline> pipeline_{};

    // Pass classes
    VulkanEngine::SceneRenderer::SceneRenderer* scene_renderer_ = nullptr;

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
