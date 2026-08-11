module;

export module VulkanEngine.Render.Passes.HiZPass;

import std;
import vulkan_hpp;
import VulkanEngine.PipelinePass;
import VulkanEngine.SceneRenderer;

export namespace VulkanEngine::SceneRenderer {

class HiZPass : public VulkanEngine::PipelinePass::IPipelinePass {
public:
    HiZPass(SceneRenderer& sr);
    ~HiZPass() override;

    HiZPass(const HiZPass&) = delete;
    HiZPass& operator=(const HiZPass&) = delete;

    void Setup(VulkanEngine::PipelinePass::PassSetupContext& ctx) override;
    void Execute(const VulkanEngine::PipelinePass::FrameContext& ctx,
                 vk::CommandBuffer cmd) override;

private:
    SceneRenderer& scene_renderer_;
};

} // namespace VulkanEngine::SceneRenderer
