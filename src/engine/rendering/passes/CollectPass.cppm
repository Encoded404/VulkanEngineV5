module;

export module VulkanEngine.Render.Passes.CollectPass;

import std;
import vulkan_hpp;
import VulkanBackend.Runtime.VulkanBootstrap;
import VulkanEngine.GpuResources;
import VulkanEngine.GpuResources.BlockArray;
import VulkanEngine.PipelinePass;
import VulkanEngine.SceneRenderer;

export namespace VulkanEngine::SceneRenderer {

inline constexpr std::uint32_t MAX_TECHNIQUES = 256;
inline constexpr std::uint32_t MAX_BLOCKS = 1024;

struct CollectPC { std::uint32_t cnt; std::uint32_t p0; std::uint32_t mt; std::uint32_t pass; };
struct WritePC { std::uint32_t cnt; std::uint32_t p0; std::uint32_t techniqueCount; std::uint32_t p1; };

class CollectPass : public VulkanEngine::PipelinePass::IPipelinePass {
public:
    CollectPass(SceneRenderer& sr);
    ~CollectPass() override;
    CollectPass(const CollectPass&) = delete;
    CollectPass& operator=(const CollectPass&) = delete;

    bool Create(VulkanBackend::Runtime::IVulkanBootstrap& backend);
    void Shutdown();

    void Execute(vk::CommandBuffer cmd,
                 vk::DescriptorSet collect_set,
                 vk::DescriptorSet collect_write_set,
                 std::uint32_t entity_count,
                 const vk::raii::Device& dev,
                 VulkanEngine::GpuResources::GpuBuffer& intermediate_buffer,
                 VulkanEngine::GpuResources::GpuBuffer& tech_counts_buffer,
                 VulkanEngine::GpuResources::GpuBuffer& tech_offsets_buffer,
                 VulkanEngine::GpuResources::GpuBuffer& technique_draw_commands,
                 VulkanEngine::GpuResources::GpuBuffer& indirection_buffer,
                 VulkanEngine::GpuResources::GpuBuffer& compacted_indirection_buffer,
                 VulkanEngine::GpuResources::BlockArray& submesh_cull);

    [[nodiscard]] vk::DescriptorSetLayout GetCountCompactLayout() const { return *collect_layout_; }
    [[nodiscard]] vk::DescriptorSetLayout GetWriteLayout() const { return *write_layout_; }

    // IPipelinePass overrides
    void Setup(VulkanEngine::PipelinePass::PassSetupContext& ctx) override;
    void Execute(const VulkanEngine::PipelinePass::FrameContext& ctx,
                 vk::CommandBuffer cmd) override;

private:
    SceneRenderer& scene_renderer_;

    std::unique_ptr<vk::raii::DescriptorSetLayout> collect_layout_{};
    std::unique_ptr<vk::raii::DescriptorSetLayout> write_layout_{};
    std::shared_ptr<VulkanEngine::GpuResources::DescriptorPool> collect_pool_;
    std::shared_ptr<VulkanEngine::GpuResources::DescriptorPool> write_pool_;
    std::unique_ptr<vk::raii::PipelineLayout> count_pipeline_layout_{};
    std::unique_ptr<vk::raii::Pipeline> count_pipeline_{};
    std::unique_ptr<vk::raii::PipelineLayout> write_pipeline_layout_{};
    std::unique_ptr<vk::raii::Pipeline> write_pipeline_{};
};

} // namespace VulkanEngine::SceneRenderer
