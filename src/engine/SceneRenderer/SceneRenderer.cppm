module;

#include <cstdint>
#include <vector>
#include <array>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_RADIANS
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <vulkan/vulkan.hpp>

export module VulkanEngine.SceneRenderer;

export import VulkanBackend.Component;
export import VulkanBackend.Runtime.VulkanBootstrap;
export import VulkanEngine.Components.Transform;
export import VulkanEngine.Components.MeshReference;
export import VulkanEngine.Components.Material;
export import VulkanEngine.GpuResources;
export import VulkanEngine.StandardMeshPipeline;
export import VulkanEngine.TechniqueManager;
export import VulkanEngine.BindlessManager;

export namespace VulkanEngine::SceneRenderer {

struct alignas(16) InstanceData {
    glm::mat4 model_matrix;
    uint32_t technique_id;
    uint32_t pad[3];
};

struct alignas(4) MeshData {
    uint32_t index_count;
    uint32_t vertex_count;
    uint32_t first_index;
    int32_t  vertex_offset;
};

struct CameraUBO {
    glm::mat4 view_proj;
    glm::vec4 frustum_planes[6];
};

struct TechniqueDrawRange {
    uint16_t technique_id;
    uint32_t first_entity;
    uint32_t entity_count;
};

class SceneRenderer {
public:
    static constexpr uint32_t MAX_ENTITIES = 4096;
    static constexpr uint32_t FRAMES_IN_FLIGHT = 3;

    SceneRenderer() = default;
    ~SceneRenderer();

    SceneRenderer(const SceneRenderer&) = delete;
    SceneRenderer& operator=(const SceneRenderer&) = delete;

    bool Initialize(VulkanEngine::Runtime::IVulkanBootstrapBackend& backend,
                    uint32_t total_vertex_count);
    void Shutdown();

    [[nodiscard]] vk::DescriptorSetLayout* GetInstanceDataLayout() const;
    [[nodiscard]] vk::DescriptorSetLayout* GetExpandedDataLayout() const;
    [[nodiscard]] vk::DescriptorSetLayout* GetCameraDataLayout() const;

    void PrepareCompute(vk::CommandBuffer cmd,
                        VulkanEngine::ComponentRegistry& registry,
                        const VulkanEngine::GpuResources::GpuBuffer& raw_vertex_buffer,
                        const VulkanEngine::GpuResources::GpuBuffer& original_index_buffer,
                        const glm::mat4& view_matrix,
                        const glm::mat4& projection_matrix,
                        uint32_t width,
                        uint32_t height,
                        uint32_t frame_index);

    void DepthPrepass(vk::CommandBuffer cmd,
                      uint32_t width,
                      uint32_t height,
                      uint32_t frame_index);

    void Render(vk::CommandBuffer cmd,
                VulkanEngine::ComponentRegistry& registry,
                const VulkanEngine::GpuResources::GpuBuffer& vertex_buffer,
                const VulkanEngine::GpuResources::GpuBuffer& index_buffer,
                VulkanEngine::TechniqueManager::TechniqueManager& technique_mgr,
                VulkanEngine::BindlessManager::BindlessManager& bindless_mgr,
                const glm::mat4& projection_matrix,
                const glm::mat4& view_matrix,
                uint32_t width,
                uint32_t height,
                uint32_t frame_index);

    [[nodiscard]] uint32_t GetCurrentEntityCount() const { return current_entity_count_; }
    [[nodiscard]] const std::vector<TechniqueDrawRange>& GetCurrentRanges() const { return current_ranges_; }

private:
    struct FrameResources {
        VulkanEngine::GpuResources::GpuBuffer instance_buffer{};
        VulkanEngine::GpuResources::GpuBuffer indirect_buffer{};          // CPU-written, main pass
        VulkanEngine::GpuResources::GpuBuffer depth_indirect_buffer{};    // GPU-written by expand, depth pass
        VulkanEngine::GpuResources::GpuBuffer mesh_data_buffer{};
        VulkanEngine::GpuResources::GpuBuffer expanded_position_buffer{};
        VulkanEngine::GpuResources::GpuBuffer expanded_attribute_buffer{};
        VulkanEngine::GpuResources::GpuBuffer expanded_index_buffer{};
        VulkanEngine::GpuResources::GpuBuffer expand_counter_buffer{};
        VulkanEngine::GpuResources::GpuBuffer camera_buffer{};
        VulkanEngine::GpuResources::GpuDescriptorSet instance_descriptor_set{};
        VulkanEngine::GpuResources::GpuDescriptorSet expanded_descriptor_set{};
        VulkanEngine::GpuResources::GpuDescriptorSet camera_descriptor_set{};
    };

    bool CreateExpandPipeline(VulkanEngine::Runtime::IVulkanBootstrapBackend& backend);
    bool CreateDepthPipeline(VulkanEngine::Runtime::IVulkanBootstrapBackend& backend);
    void DispatchExpand(vk::CommandBuffer cmd, uint32_t entity_count, uint32_t frame_index);

    VulkanEngine::Runtime::IVulkanBootstrapBackend* backend_ = nullptr;

    // Descriptor set layouts — set numbers match pipeline layouts
    // Set 0: Instance data (shared: expand compute set 0, main pass set 1)
    std::unique_ptr<vk::raii::DescriptorSetLayout> instance_data_layout_{};
    std::shared_ptr<VulkanEngine::GpuResources::DescriptorPool> instance_pool_;

    // Set 1: Expanded buffers (expand compute set 1, depth pass set 0)
    std::unique_ptr<vk::raii::DescriptorSetLayout> expanded_data_layout_{};
    std::shared_ptr<VulkanEngine::GpuResources::DescriptorPool> expanded_pool_;

    // Set 2: Camera + mesh + raw vertex (expand compute set 2, depth pass set 1)
    std::unique_ptr<vk::raii::DescriptorSetLayout> camera_data_layout_{};
    std::shared_ptr<VulkanEngine::GpuResources::DescriptorPool> camera_pool_;

    // Expand compute pipeline
    std::unique_ptr<vk::raii::PipelineLayout> compute_pipeline_layout_{};
    std::unique_ptr<vk::raii::Pipeline> compute_pipeline_{};
    std::shared_ptr<VulkanEngine::GpuResources::DescriptorPool> compute_pool_;

    // Depth pre-pass pipeline
    std::unique_ptr<vk::raii::PipelineLayout> depth_pipeline_layout_{};
    std::unique_ptr<vk::raii::Pipeline> depth_pipeline_{};

    uint32_t total_vertex_count_ = 0;

    std::array<FrameResources, FRAMES_IN_FLIGHT> frames_;
    std::vector<TechniqueDrawRange> current_ranges_{};
    uint32_t current_entity_count_ = 0;
};

} // namespace VulkanEngine::SceneRenderer
