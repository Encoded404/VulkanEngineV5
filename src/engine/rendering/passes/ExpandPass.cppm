module;

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_RADIANS
#include <glm/glm.hpp>

export module VulkanEngine.Render.Passes.ExpandPass;

import std;
import vulkan_hpp;
import VulkanBackend.Runtime.VulkanBootstrap;
import VulkanEngine.GpuResources;
import VulkanEngine.PipelinePass;

export namespace VulkanEngine::SceneRenderer {

inline constexpr std::uint32_t MAX_BLOCKS = 1024;

struct ExpandPC { glm::mat4 vp; std::uint32_t cnt; std::uint32_t p0; std::uint32_t p1; };

class ExpandPass : public VulkanEngine::PipelinePass::IPipelinePass {
public:
    ExpandPass() = default;
    ~ExpandPass() override;

    ExpandPass(const ExpandPass&) = delete;
    ExpandPass& operator=(const ExpandPass&) = delete;

    bool Create(VulkanBackend::Runtime::IVulkanBootstrap& backend,
                vk::DescriptorSetLayout bindless_index_layout);
    void Shutdown();

    void Execute(vk::CommandBuffer cmd,
                 vk::DescriptorSet expand_set,
                 vk::DescriptorSet bindless_index_set,
                 std::uint32_t object_count,
                 const glm::mat4& view_proj);

    [[nodiscard]] vk::DescriptorSetLayout GetDescriptorSetLayout() const {
        return *descriptor_layout_;
    }

    // IPipelinePass overrides
    void Setup(VulkanEngine::PipelinePass::PassSetupContext& ctx) override;
    void Execute(const VulkanEngine::PipelinePass::FrameContext& ctx,
                 vk::CommandBuffer cmd) override;

private:
    std::unique_ptr<vk::raii::DescriptorSetLayout> descriptor_layout_{};
    std::shared_ptr<VulkanEngine::GpuResources::DescriptorPool> descriptor_pool_;
    std::unique_ptr<vk::raii::PipelineLayout> pipeline_layout_{};
    std::unique_ptr<vk::raii::Pipeline> pipeline_{};
};

} // namespace VulkanEngine::SceneRenderer
