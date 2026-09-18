module;

export module VulkanEngine.Render.Passes.CollectPass;

import std;
import vulkan_hpp;
import VulkanEngine.PipelinePass;
import VulkanEngine.SceneRenderer;

export namespace VulkanEngine::SceneRenderer {

inline constexpr std::uint32_t MAX_TECHNIQUES = 256;

struct CollectPC { std::uint32_t cnt; std::uint32_t p0; std::uint32_t mt; std::uint32_t pass; };
struct WritePC { std::uint32_t cnt; std::uint32_t p0; std::uint32_t techniqueCount; std::uint32_t p1; };

class CollectPass : public VulkanEngine::PipelinePass::IPipelinePass {
public:
    CollectPass(SceneRenderer& sr);
    ~CollectPass() override;

    CollectPass(const CollectPass&) = delete;
    CollectPass& operator=(const CollectPass&) = delete;

    // IPipelinePass overrides
    void Setup(VulkanEngine::PipelinePass::PassSetupContext& ctx) override;
    void Execute(const VulkanEngine::PipelinePass::FrameContext& ctx,
                 vk::CommandBuffer cmd) override;

private:
    SceneRenderer& scene_renderer_;
};

} // namespace VulkanEngine::SceneRenderer
