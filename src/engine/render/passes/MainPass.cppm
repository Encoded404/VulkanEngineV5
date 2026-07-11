module;

export module VulkanEngine.Render.Passes.MainPass;

import std;
import vulkan_hpp;
import VulkanBackend.Vulkan.VulkanBootstrap;
import VulkanEngine.ECS.ComponentRegistry;
import VulkanEngine.TechniqueManager;
import VulkanEngine.BindlessManager;
import VulkanEngine.PipelinePass;
import VulkanEngine.SceneRenderer;

export namespace VulkanEngine::SceneRenderer {

class MainPass : public VulkanEngine::PipelinePass::IPipelinePass {
public:
    MainPass(SceneRenderer& sr);
    ~MainPass() override = default;

    void Execute(vk::CommandBuffer cmd,
                 VulkanEngine::TechniqueManager::TechniqueManager& technique_mgr,
                 VulkanEngine::BindlessManager::BindlessManager& bindless_mgr,
                 vk::DescriptorSet submesh_vertex_set,
                 vk::DescriptorSet bindless_vertex_set,
                 vk::DescriptorSet indirection_raw_set,
                 vk::Buffer technique_draw_commands_buffer,
                 std::uint32_t width, std::uint32_t height,
                 std::uint32_t entity_count);

    // IPipelinePass overrides
    void Setup(VulkanEngine::PipelinePass::PassSetupContext& ctx) override;
    void Execute(const VulkanEngine::PipelinePass::FrameContext& ctx,
                 vk::CommandBuffer cmd) override;

private:
    SceneRenderer& scene_renderer_;
};

} // namespace VulkanEngine::SceneRenderer
