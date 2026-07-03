module;

export module VulkanEngine.Render.Passes.OcclusionPass;

import std;
import vulkan_hpp;
import VulkanBackend.Runtime.VulkanBootstrap;
import VulkanEngine.GpuResources;
import VulkanEngine.PipelinePass;
import VulkanEngine.SceneRenderer;

export namespace VulkanEngine::SceneRenderer {

struct OccPC { std::uint32_t cnt; std::uint32_t refineLevel; std::uint32_t hizWidth; std::uint32_t hizHeight; };

class OcclusionPass : public VulkanEngine::PipelinePass::IPipelinePass {
public:
    OcclusionPass(SceneRenderer& sr);
    ~OcclusionPass() override;
    OcclusionPass(const OcclusionPass&) = delete;
    OcclusionPass& operator=(const OcclusionPass&) = delete;

    bool Create(VulkanBackend::Runtime::IVulkanBootstrap& backend);
    void Shutdown();

    void Execute(vk::CommandBuffer cmd, vk::DescriptorSet occlusion_set,
                 std::uint32_t entity_count,
                 std::uint32_t hiz_width, std::uint32_t hiz_height);

    [[nodiscard]] vk::DescriptorSetLayout GetDescriptorSetLayout() const { return *descriptor_layout_; }

    // IPipelinePass overrides
    void Setup(VulkanEngine::PipelinePass::PassSetupContext& ctx) override;
    void Execute(const VulkanEngine::PipelinePass::FrameContext& ctx,
                 vk::CommandBuffer cmd) override;

private:
    SceneRenderer& scene_renderer_;

    std::unique_ptr<vk::raii::DescriptorSetLayout> descriptor_layout_{};
    std::shared_ptr<VulkanEngine::GpuResources::DescriptorPool> descriptor_pool_;
    std::unique_ptr<vk::raii::PipelineLayout> pipeline_layout_{};
    std::unique_ptr<vk::raii::Pipeline> pipeline_{};
};

} // namespace VulkanEngine::SceneRenderer
