module;

export module VulkanEngine.Render.Passes.OutputPass;

import std;

import vulkan;

import VulkanEngine.PipelinePass;
import VulkanBackend.Vulkan.VulkanBootstrap;

export namespace VulkanEngine::SceneRenderer {

// Final display encode. Everything up to and including ImGui renders into the linear FP16 scene target; this pass is the only writer of the swapchain.
// It applies exposure, a display-referred tone curve for HDR, the output gamut matrix where needed, and exactly one transfer function.
// The pipeline's color format is inferred from the backbuffer attachment, so switching SDR/HDR only invalidates this pass, never a scene pipeline.
// The four transfer cases are:
// _SRGB image: linear values, hardware applies the sRGB OETF.
// UNORM/FP16 + SRGB: linear values, this shader applies the sRGB OETF.
// FP16 + EXTENDED_SRGB: linear scRGB values.
// 10-bit + HDR10_ST2084: Rec.2020 matrix plus the PQ OETF.
class OutputPass : public VulkanEngine::PipelinePass::IPipelinePass {
public:
    OutputPass(VulkanBackend::Vulkan::IVulkanBootstrap* bootstrap,
               std::uint64_t vertex_shader, std::uint64_t fragment_shader);
    ~OutputPass() override;

    OutputPass(const OutputPass&) = delete;
    OutputPass& operator=(const OutputPass&) = delete;

    [[nodiscard]] std::string_view GetName() const override { return "output-present"; }

    void Setup(VulkanEngine::PipelinePass::PassSetupContext& ctx) override;
    void Execute(const VulkanEngine::PipelinePass::FrameContext& ctx,
                 vk::CommandBuffer cmd) override;

private:
    VulkanBackend::Vulkan::IVulkanBootstrap* bootstrap_ = nullptr;
    std::uint64_t vertex_shader_ = 0;
    std::uint64_t fragment_shader_ = 0;
};

} // namespace VulkanEngine::SceneRenderer
