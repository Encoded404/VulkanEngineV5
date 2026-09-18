module;

export module VulkanEngine.Render.Passes.DepthPrePass;

import std;
import vulkan_hpp;
import VulkanEngine.PipelinePass;
import VulkanEngine.SceneRenderer;

export namespace VulkanEngine::SceneRenderer {

class DepthPrePass : public VulkanEngine::PipelinePass::IPipelinePass {
public:
    DepthPrePass(SceneRenderer& sr);
    ~DepthPrePass() override;

    DepthPrePass(const DepthPrePass&) = delete;
    DepthPrePass& operator=(const DepthPrePass&) = delete;

    void Setup(VulkanEngine::PipelinePass::PassSetupContext& ctx) override;
    void Execute(const VulkanEngine::PipelinePass::FrameContext& ctx,
                 vk::CommandBuffer cmd) override;

private:
    SceneRenderer& scene_renderer_;
};

} // namespace VulkanEngine::SceneRenderer
