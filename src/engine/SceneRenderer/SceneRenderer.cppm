module;

#include <cstdint>
#include <vector>
#include <array>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_RADIANS
#include <glm/glm.hpp> //NOLINT(misc-include-cleaner)
#include <glm/gtc/matrix_transform.hpp> //NOLINT(misc-include-cleaner)
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
    uint32_t material_id;
    uint32_t pad[3]; //NOLINT(modernize-avoid-c-arrays)
};

struct alignas(4) MeshData {
    uint32_t index_count;
    uint32_t instance_count; // always 1
    uint32_t first_index;
    int32_t  vertex_offset;
};

class SceneRenderer {
public:
    static constexpr uint32_t MAX_ENTITIES = 4096;
    static constexpr uint32_t FRAMES_IN_FLIGHT = 3;

    SceneRenderer() = default;
    ~SceneRenderer();

    SceneRenderer(const SceneRenderer&) = delete;
    SceneRenderer& operator=(const SceneRenderer&) = delete;

    bool Initialize(VulkanEngine::Runtime::IVulkanBootstrapBackend& backend);
    void Shutdown();

    [[nodiscard]] vk::DescriptorSetLayout* GetInstanceDataLayout() const;

    // Must be called BEFORE the render pass begins — dispatches compute indirect generation
    void PrepareCompute(vk::CommandBuffer cmd,
                        VulkanEngine::ComponentRegistry& registry,
                        uint32_t frame_index);

    // Called inside the render pass — draws using GPU-generated indirect commands
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

private:
    struct FrameResources {
        // Graphics resources
        VulkanEngine::GpuResources::GpuBuffer instance_buffer{};
        VulkanEngine::GpuResources::GpuBuffer indirect_buffer{};
        VulkanEngine::GpuResources::GpuBuffer mesh_data_buffer{};
        VulkanEngine::GpuResources::GpuDescriptorSet gfx_descriptor_set{};
        // Compute resources
        VulkanEngine::GpuResources::GpuDescriptorSet compute_descriptor_set{};
    };

    bool CreateComputePipeline(VulkanEngine::Runtime::IVulkanBootstrapBackend& backend);
    void DispatchCompute(vk::CommandBuffer cmd, uint32_t entity_count, uint32_t frame_index);

    VulkanEngine::Runtime::IVulkanBootstrapBackend* backend_ = nullptr;

    // Instance data descriptor set (graphics, set=1)
    std::unique_ptr<vk::raii::DescriptorSetLayout> instance_data_layout_{};
    std::shared_ptr<VulkanEngine::GpuResources::DescriptorPool> gfx_pool_;

    // Compute pipeline
    std::unique_ptr<vk::raii::DescriptorSetLayout> compute_layout_{};
    std::unique_ptr<vk::raii::PipelineLayout> compute_pipeline_layout_{};
    std::unique_ptr<vk::raii::Pipeline> compute_pipeline_{};
    std::shared_ptr<VulkanEngine::GpuResources::DescriptorPool> compute_pool_;

    struct TechniqueDrawRange {
        uint16_t technique_id;
        uint32_t first_entity;
        uint32_t entity_count;
    };

    std::array<FrameResources, FRAMES_IN_FLIGHT> frames_;
    std::vector<TechniqueDrawRange> current_ranges_{};
    uint32_t current_entity_count_ = 0;
};

}
