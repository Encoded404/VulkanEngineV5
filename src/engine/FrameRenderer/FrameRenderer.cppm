module;

#include <vector>

#include <vulkan/vulkan.hpp>

export module VulkanEngine.FrameRenderer;

export import VulkanBackend.Runtime.VulkanBootstrap;
export import VulkanEngine.RenderPipeline;
export import VulkanEngine.ImGuiSystem;

export namespace VulkanEngine::FrameRenderer {

struct FrameRendererConfig {
    bool enable_imgui = true;
};

class FrameRenderer {
public:
    void Initialize(VulkanEngine::Runtime::VulkanBootstrap& bootstrap,
                    VulkanEngine::RenderPipeline::RenderPipeline& pipeline,
                    const FrameRendererConfig& config);
    void Shutdown();

    void RenderFrame(VulkanEngine::RenderPipeline::RenderPipeline& pipeline,
                     const void* pass_user_data,
                     VulkanEngine::ImGuiSystem::ImGuiSystem* imgui,
                     uint32_t image_index);

private:
    VulkanEngine::Runtime::VulkanBootstrap* bootstrap_ = nullptr;
    std::vector<bool> swapchain_image_presented_{};
    bool depth_buffer_initialized_ = false;
    bool enable_imgui_ = true;
    uint32_t backbuffer_resource_index_ = 0;
    uint32_t depth_buffer_resource_index_ = 0;
};

} // namespace VulkanEngine::FrameRenderer
