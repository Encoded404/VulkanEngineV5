module;

#include <cstdint>
#include <memory>
#include <vector>
#include <vulkan/vulkan_raii.hpp>

export module VulkanEngine.TechniqueManager;

export import VulkanBackend.Runtime.VulkanBootstrap;
export import VulkanEngine.StandardMeshPipeline;

export namespace VulkanEngine::TechniqueManager {

class TechniqueManager {
public:
    TechniqueManager() = default;
    ~TechniqueManager();

    TechniqueManager(const TechniqueManager&) = delete;
    TechniqueManager& operator=(const TechniqueManager&) = delete;

    [[nodiscard]] uint16_t RegisterTechnique(VulkanEngine::Runtime::VulkanBootstrap& bootstrap,
                                              const std::vector<uint32_t>& vert_spv,
                                              const std::vector<uint32_t>& frag_spv,
                                              const VulkanEngine::StandardMeshPipeline::PipelineConfig& config = {},
                                              vk::DescriptorSetLayout* bindless_layout = nullptr,
                                              vk::DescriptorSetLayout* instance_data_layout = nullptr);

    [[nodiscard]] VulkanEngine::StandardMeshPipeline::PipelineManager* GetPipelineManager(uint16_t technique_id);

    [[nodiscard]] uint16_t GetTechniqueCount() const { return static_cast<uint16_t>(techniques_.size()); }

    void Shutdown();

private:
    struct Technique {
        std::unique_ptr<VulkanEngine::StandardMeshPipeline::PipelineManager> pipeline_manager;
    };

    std::vector<Technique> techniques_{};
};

}
