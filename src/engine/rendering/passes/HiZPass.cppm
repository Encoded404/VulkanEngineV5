module;

export module VulkanEngine.Render.Passes.HiZPass;

import std;
import vulkan_hpp;
import VulkanBackend.Runtime.VulkanBootstrap;
import VulkanEngine.GpuResources;
import VulkanEngine.PipelinePass;

export namespace VulkanEngine::SceneRenderer {

inline constexpr std::uint32_t MAX_HIZ_MIPS = 12;
inline constexpr std::uint32_t HIZ_BATCH = 2;

struct HiZPC { std::uint32_t bl; std::uint32_t sw; std::uint32_t sh; std::uint32_t tc; };

class HiZPass : public VulkanEngine::PipelinePass::IPipelinePass {
public:
    HiZPass() = default;
    ~HiZPass() override;

    HiZPass(const HiZPass&) = delete;
    HiZPass& operator=(const HiZPass&) = delete;

    bool Create(VulkanBackend::Runtime::IVulkanBootstrap& backend);
    void Shutdown();

    void Execute(vk::CommandBuffer cmd,
                 vk::DescriptorSet hiz_set,
                 std::uint32_t width,
                 std::uint32_t height,
                 std::uint32_t mip_count);

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
