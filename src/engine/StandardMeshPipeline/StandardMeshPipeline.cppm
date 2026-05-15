module;

#include <cstdint>
#include <memory>
#include <vector>

#include <vulkan/vulkan_raii.hpp>

export module VulkanEngine.StandardMeshPipeline;

export import VulkanBackend.Runtime.VulkanBootstrap;
export import VulkanEngine.GpuResources;

export namespace VulkanEngine::StandardMeshPipeline {

struct Vertex {
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
};

struct MeshGPUResources {
    VulkanEngine::GpuResources::GpuBuffer vertex_buffer{};
    VulkanEngine::GpuResources::GpuBuffer index_buffer{};
    VulkanEngine::GpuResources::GpuTexture texture{};
    uint32_t index_count = 0;
};

class PipelineManager {
public:
    PipelineManager();
    ~PipelineManager();

    void Initialize(VulkanEngine::Runtime::VulkanBootstrap& bootstrap,
                    const std::vector<uint32_t>& vertex_spirv,
                    const std::vector<uint32_t>& fragment_spirv);
    void Shutdown();

    [[nodiscard]] vk::PipelineLayout* GetPipelineLayout();
    [[nodiscard]] const vk::PipelineLayout* GetPipelineLayout() const;
    [[nodiscard]] vk::raii::Pipeline* GetPipeline();
    [[nodiscard]] const vk::raii::Pipeline* GetPipeline() const;
    [[nodiscard]] vk::DescriptorSetLayout* GetDescriptorSetLayout();
    [[nodiscard]] const vk::DescriptorSetLayout* GetDescriptorSetLayout() const;

    VulkanEngine::GpuResources::GpuDescriptorSet AllocateDescriptorSet();
    VulkanEngine::GpuResources::GpuDescriptorSet AllocateDescriptorSet(const VulkanEngine::GpuResources::GpuTexture& texture);

private:
    void CreatePipeline(VulkanEngine::Runtime::VulkanBootstrap& bootstrap,
                        const std::vector<uint32_t>& vertex_spirv,
                        const std::vector<uint32_t>& fragment_spirv);
    void CreateDescriptorSetLayout(VulkanEngine::Runtime::VulkanBootstrap& bootstrap);
    void CreateDescriptorPool(VulkanEngine::Runtime::VulkanBootstrap& bootstrap);

    VulkanEngine::Runtime::VulkanBootstrap* bootstrap_ = nullptr;
    std::shared_ptr<VulkanEngine::GpuResources::DescriptorPool> pool_;
    std::unique_ptr<vk::raii::DescriptorSetLayout> descriptor_set_layout_{};
    std::unique_ptr<vk::raii::PipelineLayout> pipeline_layout_{};
    std::unique_ptr<vk::raii::Pipeline> pipeline_{};
};

} // namespace VulkanEngine::StandardMeshPipeline
