module;

export module VulkanEngine.Render.Passes.OccluderPrePass;

import std;
import vulkan_hpp;
import VulkanEngine.PipelinePass;
import VulkanEngine.SceneRenderer;

export namespace VulkanEngine::SceneRenderer {

class OccluderPrePass : public VulkanEngine::PipelinePass::IPipelinePass {
public:
    OccluderPrePass(SceneRenderer& sr);
    ~OccluderPrePass() override;

    OccluderPrePass(const OccluderPrePass&) = delete;
    OccluderPrePass& operator=(const OccluderPrePass&) = delete;

    void Setup(VulkanEngine::PipelinePass::PassSetupContext& ctx) override;
    void Execute(const VulkanEngine::PipelinePass::FrameContext& ctx,
                 vk::CommandBuffer cmd) override;

private:
    SceneRenderer& scene_renderer_;
};

} // namespace VulkanEngine::SceneRenderer
