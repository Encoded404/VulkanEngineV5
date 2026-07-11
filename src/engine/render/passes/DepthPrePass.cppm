module;

export module VulkanEngine.Render.Passes.DepthPrePass;

import std;
import vulkan_hpp;
import VulkanBackend.Vulkan.VulkanBootstrap;
import VulkanEngine.PipelinePass;
import VulkanEngine.SceneRenderer;

export namespace VulkanEngine::SceneRenderer {

class DepthPrePass : public VulkanEngine::PipelinePass::IPipelinePass {
public:
    DepthPrePass(SceneRenderer& sr);
    ~DepthPrePass() override;
    DepthPrePass(const DepthPrePass&) = delete;
    DepthPrePass& operator=(const DepthPrePass&) = delete;

    bool Create(VulkanBackend::Vulkan::IVulkanBootstrap& backend,
                vk::DescriptorSetLayout empty_layout,
                vk::DescriptorSetLayout submesh_vertex_layout,
                vk::DescriptorSetLayout raw_vertex_layout,
                vk::DescriptorSetLayout indirection_layout,
                const vk::PipelineRasterizationStateCreateInfo& rasterization);
    void Shutdown();

    void Execute(vk::CommandBuffer cmd,
                 std::uint32_t width, std::uint32_t height,
                 vk::DescriptorSet empty_set,
                 vk::DescriptorSet submesh_vertex_set,
                 vk::DescriptorSet bindless_vertex_set,
                 vk::DescriptorSet depth_indirection_set,
                 vk::Buffer draw_count_buffer);

    [[nodiscard]] vk::Pipeline GetPipeline() const { return *pipeline_; }
    [[nodiscard]] vk::PipelineLayout GetPipelineLayout() const { return *pipeline_layout_; }

    // IPipelinePass overrides
    void Setup(VulkanEngine::PipelinePass::PassSetupContext& ctx) override;
    void Execute(const VulkanEngine::PipelinePass::FrameContext& ctx,
                 vk::CommandBuffer cmd) override;

private:
    SceneRenderer& scene_renderer_;

    std::unique_ptr<vk::raii::PipelineLayout> pipeline_layout_{};
    std::unique_ptr<vk::raii::Pipeline> pipeline_{};
};

} // namespace VulkanEngine::SceneRenderer
