module;

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>
#include <vulkan/vulkan_raii.hpp>

export module VulkanEngine.TechniqueManager;

export import VulkanBackend.Runtime.VulkanBootstrap;
export import VulkanEngine.StandardMeshPipeline;

export namespace VulkanEngine::TechniqueManager {

struct ShaderOverride {
    std::vector<uint32_t> vertex_spv;
    std::vector<uint32_t> fragment_spv;
};

class TechniqueManager {
public:
    TechniqueManager() = default;
    ~TechniqueManager();

    TechniqueManager(const TechniqueManager&) = delete;
    TechniqueManager& operator=(const TechniqueManager&) = delete;

    using TechniqueChangedCallback = std::function<void(uint16_t id, VkPipeline pipeline, VkPipelineLayout layout)>;

    void SetTechniqueCallback(TechniqueChangedCallback callback) { on_technique_changed_ = std::move(callback); }

    [[nodiscard]] uint16_t RegisterTechnique(VulkanEngine::Runtime::VulkanBootstrap& bootstrap,
                                              const std::vector<uint32_t>& vert_spv,
                                              const std::vector<uint32_t>& frag_spv,
                                              const VulkanEngine::StandardMeshPipeline::PipelineConfig& config = {},
                                              vk::DescriptorSetLayout* bindless_layout = nullptr,
                                              vk::DescriptorSetLayout* object_data_layout = nullptr,
                                              vk::DescriptorSetLayout* raw_vertex_layout = nullptr,
                                              vk::DescriptorSetLayout* indirection_layout = nullptr);

    [[nodiscard]] uint16_t RegisterTechnique(VulkanEngine::Runtime::VulkanBootstrap& bootstrap,
                                              const ShaderOverride& override,
                                              const VulkanEngine::StandardMeshPipeline::PipelineConfig& config = {},
                                              vk::DescriptorSetLayout* bindless_layout = nullptr,
                                              vk::DescriptorSetLayout* object_data_layout = nullptr,
                                              vk::DescriptorSetLayout* raw_vertex_layout = nullptr,
                                              vk::DescriptorSetLayout* indirection_layout = nullptr);

    [[nodiscard]] VulkanEngine::StandardMeshPipeline::GraphicsPipeline* GetGraphicsPipeline(uint16_t technique_id);

    [[nodiscard]] uint16_t GetTechniqueCount() const { return static_cast<uint16_t>(techniques_.size()); }

    void Shutdown();

private:
    struct Technique {
        std::unique_ptr<VulkanEngine::StandardMeshPipeline::GraphicsPipeline> graphics_pipeline;
    };

    TechniqueChangedCallback on_technique_changed_{};
    std::vector<Technique> techniques_{};
    std::vector<uint32_t> default_vert_spv_{};
    std::vector<uint32_t> default_frag_spv_{};
    bool has_defaults_ = false;
};

}

