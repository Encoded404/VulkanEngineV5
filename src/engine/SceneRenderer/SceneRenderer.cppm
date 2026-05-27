module;

#include <cstdint>
#include <vector>
#include <array>
#include <memory>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_RADIANS
#include <glm/glm.hpp> //NOLINT(misc-include-cleaner)
#include <vulkan/vulkan_raii.hpp>

export module VulkanEngine.SceneRenderer;

export import VulkanBackend.Component;
export import VulkanBackend.Runtime.VulkanBootstrap;
export import VulkanEngine.Components.Transform;
export import VulkanEngine.Components.MeshReference;
export import VulkanEngine.StandardMeshPipeline;
export import VulkanEngine.TechniqueManager;
export import VulkanEngine.BindlessManager;
export import VulkanEngine.Mesh.MeshTypes;
export import VulkanEngine.GpuResources;
export import VulkanEngine.GpuResources.BlockArray;

export namespace VulkanEngine::SceneRenderer {

class SceneRenderer {
public:
    static constexpr uint32_t FRAMES_IN_FLIGHT = 3;
    static constexpr uint32_t MAX_HIZ_MIPS = 12;
    static constexpr uint32_t MAX_VERTEX_BUFFERS = 64;
    static constexpr uint32_t MAX_INDEX_BUFFERS = 64;
    static constexpr uint32_t BLOCK_ENTRIES = 256;
    static constexpr uint32_t MAX_BLOCKS = 1024;
    static constexpr uint32_t MAX_TECHNIQUES = 256;
    static constexpr uint32_t DGC_MAX_SEQUENCES = 256;

    SceneRenderer() = default;
    ~SceneRenderer();

    SceneRenderer(const SceneRenderer&) = delete;
    SceneRenderer& operator=(const SceneRenderer&) = delete;

    bool Initialize(VulkanEngine::Runtime::IVulkanBootstrap& backend,
                    VulkanEngine::GpuResources::DeviceBufferHeap& vertex_heap,
                    uint32_t total_vertex_count,
                    uint32_t total_index_count);
    void Shutdown();

    [[nodiscard]] vk::DescriptorSetLayout* GetSubmeshVertexDataLayout() const;
    [[nodiscard]] vk::DescriptorSetLayout* GetRawVertexLayout() const;
    [[nodiscard]] vk::DescriptorSetLayout* GetIndirectionLayout() const;

    void UpdateVertexBufferArrayElement(uint32_t buffer_index, vk::Buffer buffer, uint64_t size);
    void UpdateIndexBufferArrayElement(uint32_t buffer_index, vk::Buffer buffer, uint64_t size);

    void PrepareCompute(vk::CommandBuffer cmd,
                        VulkanEngine::ComponentRegistry& registry,
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
                VulkanEngine::TechniqueManager::TechniqueManager& technique_mgr,
                VulkanEngine::BindlessManager::BindlessManager& bindless_mgr,
                const glm::mat4& projection_matrix,
                const glm::mat4& view_matrix,
                uint32_t width,
                uint32_t height,
                uint32_t frame_index);

    void DispatchHiZGen(vk::CommandBuffer cmd,
                        uint32_t width,
                        uint32_t height,
                        uint32_t frame_index,
                        uint32_t image_index);

    void DispatchOcclusion(vk::CommandBuffer cmd,
                           uint32_t frame_index);

    void DispatchCollect(vk::CommandBuffer cmd, uint32_t frame_index);

    void InitializeHizFirstFrame(vk::CommandBuffer cmd);

    void SetSubmeshes(const std::vector<VulkanEngine::SubMesh>& submeshes) { scene_submeshes_ = submeshes; }

    [[nodiscard]] uint32_t GetCurrentEntityCount() const { return current_entity_count_; }

    [[nodiscard]] VkImage GetHizImage(uint32_t frame_index) const {
        const auto& frame = frames_[frame_index % FRAMES_IN_FLIGHT];
        return static_cast<VkImage>(*frame.hiz_image);
    }
    [[nodiscard]] VkImageView GetHizFullView(uint32_t frame_index) const {
        const auto& frame = frames_[frame_index % FRAMES_IN_FLIGHT];
        return static_cast<VkImageView>(*frame.hiz_full_view);
    }
    void UpdateHizDepthBinding(uint32_t frame_index, VkImageView depth_view);

    void SetupTechniqueDgcCallback(VulkanEngine::TechniqueManager::TechniqueManager& tm);

    void DispatchExpand(vk::CommandBuffer cmd, uint32_t object_count,
                        const glm::mat4& view_proj, uint32_t frame_index);

    // Buffer access for render graph
    [[nodiscard]] VkBuffer GetTechniqueDrawCommandsBuffer(uint32_t frame_index) const {
        const auto& fr = frames_[frame_index % FRAMES_IN_FLIGHT];
        return static_cast<VkBuffer>(*fr.technique_draw_commands.GetBuffer());
    }
    [[nodiscard]] VkBuffer GetDgcSequenceBuffer(uint32_t frame_index) const {
        const auto& fr = frames_[frame_index % FRAMES_IN_FLIGHT];
        return static_cast<VkBuffer>(*fr.dgc_sequence_buffer.GetBuffer());
    }
    [[nodiscard]] VkBuffer GetDgcCountBuffer(uint32_t frame_index) const {
        const auto& fr = frames_[frame_index % FRAMES_IN_FLIGHT];
        return static_cast<VkBuffer>(*fr.dgc_count_buffer.GetBuffer());
    }
    [[nodiscard]] bool IsDgcAvailable() const { return dgc_available_; }

private:
    struct FrameResources {
        // Block-based per-submesh buffers
        VulkanEngine::GpuResources::BlockArray compact_dynamic{};
        VulkanEngine::GpuResources::BlockArray compact_static{};
        VulkanEngine::GpuResources::BlockArray bounding_spheres{};
        VulkanEngine::GpuResources::BlockArray bounding_obb{};
        VulkanEngine::GpuResources::BlockArray submesh_vertex_data{};
        VulkanEngine::GpuResources::BlockArray submesh_cull{};

        // Single buffers for indirection, draw commands
        VulkanEngine::GpuResources::GpuBuffer indirection_buffer{};
        VulkanEngine::GpuResources::GpuBuffer compacted_indirection_buffer{};
        VulkanEngine::GpuResources::GpuBuffer draw_count_buffer{};
        VulkanEngine::GpuResources::GpuBuffer technique_draw_commands{};

        // DGC buffers (only used when DGC is available)
        VulkanEngine::GpuResources::GpuBuffer intermediate_buffer{};
        VulkanEngine::GpuResources::GpuBuffer dgc_sequence_buffer{};
        VulkanEngine::GpuResources::GpuBuffer dgc_count_buffer{};
        VulkanEngine::GpuResources::GpuBuffer dgc_preprocess_buffer{};
        uint64_t dgc_preprocess_size = 0;

        // Descriptor sets
        VulkanEngine::GpuResources::GpuDescriptorSet expand_set{};
        VulkanEngine::GpuResources::GpuDescriptorSet occlusion_set{};
        VulkanEngine::GpuResources::GpuDescriptorSet collect_set{};
        VulkanEngine::GpuResources::GpuDescriptorSet collect_write_set{};
        VulkanEngine::GpuResources::GpuDescriptorSet submesh_vertex_set{};
        vk::raii::DescriptorSet indirection_raw_set = vk::raii::DescriptorSet(nullptr);
        vk::raii::DescriptorSet depth_indirection_set = vk::raii::DescriptorSet(nullptr);
        VulkanEngine::GpuResources::GpuDescriptorSet hiz_set{};

        vk::raii::Image hiz_image = vk::raii::Image(nullptr);
        vk::raii::DeviceMemory hiz_memory = vk::raii::DeviceMemory(nullptr);
        std::vector<vk::raii::ImageView> hiz_mip_views{};
        vk::raii::ImageView hiz_full_view = vk::raii::ImageView(nullptr);
    };

    bool CreateExpandPipeline(const VulkanEngine::Runtime::IVulkanBootstrap& backend);
    bool CreateDepthPipeline(VulkanEngine::Runtime::IVulkanBootstrap& backend,
                             const vk::PipelineRasterizationStateCreateInfo& rasterization);
    bool CreateHiZPipeline(VulkanEngine::Runtime::IVulkanBootstrap& backend);
    bool CreateOcclusionPipeline(const VulkanEngine::Runtime::IVulkanBootstrap& backend);
    bool CreateCollectPipelines(const VulkanEngine::Runtime::IVulkanBootstrap& backend);
    bool CreateDegeneratePipeline(const VulkanEngine::Runtime::IVulkanBootstrap& backend);


    void UpdateBlockArrayDescriptor(vk::DescriptorSet desc_set, uint32_t binding,
                                      VulkanEngine::GpuResources::BlockArray& block_buf,
                                      vk::DescriptorType desc_type);

    VulkanEngine::Runtime::IVulkanBootstrap* backend_ = nullptr;

    // Set 1: SubmeshVertexData blocks
    std::unique_ptr<vk::raii::DescriptorSetLayout> submesh_vertex_layout_{};
    std::shared_ptr<VulkanEngine::GpuResources::DescriptorPool> submesh_vertex_pool_;

    // Set 2: Bindless vertex buffer array
    std::unique_ptr<vk::raii::DescriptorSetLayout> raw_vertex_layout_{};
    std::unique_ptr<vk::raii::DescriptorPool> raw_vertex_pool_;
    vk::raii::DescriptorSet bindless_vertex_set_{nullptr};

    // Set 3: Indirection buffer (depth uses dedicated set, main uses indirection_raw_set)
    std::unique_ptr<vk::raii::DescriptorSetLayout> indirection_layout_{};
    std::shared_ptr<VulkanEngine::GpuResources::DescriptorPool> indirection_pool_;
    std::unique_ptr<vk::raii::DescriptorPool> indirection_raw_pool_;

    // Set 4: Expand compute (block arrays + single buffers)
    std::unique_ptr<vk::raii::DescriptorSetLayout> expand_layout_{};
    std::shared_ptr<VulkanEngine::GpuResources::DescriptorPool> expand_pool_;
    std::unique_ptr<vk::raii::PipelineLayout> expand_pipeline_layout_{};
    std::unique_ptr<vk::raii::Pipeline> expand_pipeline_{};

    // Set 5: Occlusion compute (blocks + Hi-Z)
    std::unique_ptr<vk::raii::DescriptorSetLayout> occlusion_layout_{};
    std::shared_ptr<VulkanEngine::GpuResources::DescriptorPool> occlusion_pool_;
    std::unique_ptr<vk::raii::PipelineLayout> occlusion_pipeline_layout_{};
    std::unique_ptr<vk::raii::Pipeline> occlusion_pipeline_{};

    // Set 6: Collect count + compact (cull blocks + indirections + intermediate)
    std::unique_ptr<vk::raii::DescriptorSetLayout> collect_layout_{};
    std::shared_ptr<VulkanEngine::GpuResources::DescriptorPool> collect_pool_;
    std::unique_ptr<vk::raii::PipelineLayout> collect_pipeline_layout_{};
    std::unique_ptr<vk::raii::Pipeline> collect_pipeline_{};
    // Collect write shaders (use separate layout)
    std::unique_ptr<vk::raii::DescriptorSetLayout> collect_write_layout_{};
    std::shared_ptr<VulkanEngine::GpuResources::DescriptorPool> collect_write_pool_;
    std::unique_ptr<vk::raii::PipelineLayout> collect_write_pipeline_layout_{};
    std::unique_ptr<vk::raii::Pipeline> collect_write_pipeline_{};

    std::unique_ptr<vk::raii::Sampler> depth_sampler_{};
    std::unique_ptr<vk::raii::Sampler> hiz_sampler_{};

    uint32_t depth_width_ = 0;
    uint32_t depth_height_ = 0;
    uint32_t hiz_mip_count_ = 0;
    bool hiz_initialized_ = false;
    uint32_t total_index_count_ = 0;

    std::unique_ptr<vk::raii::DescriptorSetLayout> empty_layout_{};
    std::shared_ptr<VulkanEngine::GpuResources::DescriptorPool> empty_pool_{};
    std::vector<VulkanEngine::GpuResources::GpuDescriptorSet> empty_sets_{};

    std::unique_ptr<vk::raii::PipelineLayout> depth_pipeline_layout_{};
    std::unique_ptr<vk::raii::Pipeline> depth_pipeline_{};

    std::unique_ptr<vk::raii::DescriptorSetLayout> hiz_layout_{};
    std::unique_ptr<vk::raii::PipelineLayout> hiz_pipeline_layout_{};
    std::unique_ptr<vk::raii::Pipeline> hiz_pipeline_{};
    std::shared_ptr<VulkanEngine::GpuResources::DescriptorPool> hiz_pool_;

    // Bindless index buffer array (used by expand at set 5)
    std::unique_ptr<vk::raii::DescriptorSetLayout> bindless_index_layout_{};
    std::unique_ptr<vk::raii::DescriptorPool> bindless_index_pool_;
    vk::raii::DescriptorSet bindless_index_set_{nullptr};

    // DGC objects (only created when DGC is available)
    bool dgc_available_ = false;
    uint32_t dgc_max_sequence_count_ = 0;
    std::unique_ptr<vk::raii::IndirectCommandsLayoutEXT> dgc_commands_layout_{};
    std::unique_ptr<vk::raii::IndirectExecutionSetEXT> dgc_execution_set_{};
    vk::raii::PipelineLayout dgc_degenerate_layout_ = nullptr;
    vk::raii::Pipeline dgc_degenerate_pipeline_ = nullptr;

    std::vector<VulkanEngine::SubMesh> scene_submeshes_{};

    std::array<FrameResources, FRAMES_IN_FLIGHT> frames_;
    uint32_t current_entity_count_ = 0;
};

}
