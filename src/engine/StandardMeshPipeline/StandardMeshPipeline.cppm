module;

#include <cstdint>
#include <memory>
#include <vector>

#include <vulkan/vulkan_raii.hpp>

export module VulkanEngine.StandardMeshPipeline;

export import VulkanBackend.Runtime.VulkanBootstrap;
export import VulkanEngine.GpuResources;

export namespace VulkanEngine::StandardMeshPipeline {

struct PipelineConfig {
    // Rasterization
    vk::PolygonMode polygon_mode = vk::PolygonMode::eFill;
    vk::CullModeFlags cull_mode = vk::CullModeFlagBits::eFront;
    vk::FrontFace front_face = vk::FrontFace::eCounterClockwise;
    float line_width = 1.0f;

    // Depth/stencil
    bool depth_test_enable = true;
    bool depth_write_enable = true;
    vk::CompareOp depth_compare_op = vk::CompareOp::eLess;

    // Color blend
    bool blend_enable = false;
    vk::BlendFactor src_color_blend_factor = vk::BlendFactor::eSrcAlpha;
    vk::BlendFactor dst_color_blend_factor = vk::BlendFactor::eOneMinusSrcAlpha;
    vk::BlendOp color_blend_op = vk::BlendOp::eAdd;
    vk::BlendFactor src_alpha_blend_factor = vk::BlendFactor::eOne;
    vk::BlendFactor dst_alpha_blend_factor = vk::BlendFactor::eZero;
    vk::BlendOp alpha_blend_op = vk::BlendOp::eAdd;

    // Multisample
    vk::SampleCountFlagBits sample_count = vk::SampleCountFlagBits::e1;

    // Topology
    vk::PrimitiveTopology primitive_topology = vk::PrimitiveTopology::eTriangleList;
};



struct Vertex {
    float px, py, pz;       // position  (12 bytes)
    float nx, ny, nz;       // normal    (12 bytes)
    float u, v;             // texcoord  ( 8 bytes)
    uint16_t material_id;   // material  ( 2 bytes)
    uint16_t padding;      // alignment ( 2 bytes)
    // Total: 36 bytes
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
                    const std::vector<uint32_t>& fragment_spirv,
                    const PipelineConfig& config = {},
                    vk::DescriptorSetLayout* bindless_layout = nullptr,
                    vk::DescriptorSetLayout* instance_data_layout = nullptr,
                    vk::DescriptorSetLayout* expanded_data_layout = nullptr);
    void Shutdown();

    [[nodiscard]] vk::PipelineLayout* GetPipelineLayout();
    [[nodiscard]] const vk::PipelineLayout* GetPipelineLayout() const;
    [[nodiscard]] vk::raii::Pipeline* GetPipeline();
    [[nodiscard]] const vk::raii::Pipeline* GetPipeline() const;
    [[nodiscard]] vk::DescriptorSetLayout* GetDescriptorSetLayout();
    [[nodiscard]] const vk::DescriptorSetLayout* GetDescriptorSetLayout() const;

private:
    void CreatePipeline(VulkanEngine::Runtime::VulkanBootstrap& bootstrap,
                        const std::vector<uint32_t>& vertex_spirv,
                        const std::vector<uint32_t>& fragment_spirv,
                        const PipelineConfig& config);
    void CreateDescriptorSetLayout(VulkanEngine::Runtime::VulkanBootstrap& bootstrap);
    void CreateDescriptorPool(VulkanEngine::Runtime::VulkanBootstrap& bootstrap);

    VulkanEngine::Runtime::VulkanBootstrap* bootstrap_ = nullptr;
    PipelineConfig config_{};
    vk::DescriptorSetLayout* external_layout_ = nullptr;
    vk::DescriptorSetLayout* instance_data_layout_ = nullptr;
    vk::DescriptorSetLayout* expanded_data_layout_ = nullptr;
    std::shared_ptr<VulkanEngine::GpuResources::DescriptorPool> pool_;
    std::unique_ptr<vk::raii::DescriptorSetLayout> descriptor_set_layout_{};
    std::unique_ptr<vk::raii::PipelineLayout> pipeline_layout_{};
    std::unique_ptr<vk::raii::Pipeline> pipeline_{};
};

} // namespace VulkanEngine::StandardMeshPipeline
