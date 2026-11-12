module;

export module VulkanEngine.Render.Passes.DeformPass;

import std;
import vulkan;
import VulkanEngine.PipelinePass;
import VulkanEngine.SceneRenderer;

export namespace VulkanEngine::SceneRenderer {

// The engine-owned deform pass: runs every modifier dispatch of the frame
// between expand and occluder-select.
//
// It must be a built-in pass rather than an application pass: it needs the
// engine's private per-frame buffers, its inputs are imported graph resources
// that carry no Vulkan handle, and an async-compute pass may not touch engine
// resources (docs/app-render-passes.md), so it runs on the graphics queue.
class DeformPass : public VulkanEngine::PipelinePass::IPipelinePass {
public:
    explicit DeformPass(SceneRenderer& sr);
    ~DeformPass() override;

    DeformPass(const DeformPass&) = delete;
    DeformPass& operator=(const DeformPass&) = delete;

    [[nodiscard]] std::string_view GetName() const override { return "deform"; }

    void Setup(VulkanEngine::PipelinePass::PassSetupContext& ctx) override;
    void Execute(const VulkanEngine::PipelinePass::FrameContext& ctx,
                 vk::CommandBuffer cmd) override;

private:
    SceneRenderer& scene_renderer_;
};

} // namespace VulkanEngine::SceneRenderer
