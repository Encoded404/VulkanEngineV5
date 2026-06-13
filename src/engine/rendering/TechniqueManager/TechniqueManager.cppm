module;

export module VulkanEngine.TechniqueManager;

import std;
import std.compat;

import vulkan_hpp;

export import VulkanBackend.Runtime.VulkanBootstrap;
export import VulkanBackend.Utils.CallbackList;
export import VulkanEngine.StandardMeshPipeline;

#ifndef UINT16_MAX
constexpr std::uint16_t UINT16_MAX =
    std::numeric_limits<std::uint16_t>::max();
#endif

export namespace VulkanEngine::TechniqueManager {

struct ShaderOverride {
    std::vector<std::uint32_t> vertex_spv;
    std::vector<std::uint32_t> fragment_spv;
};

class TechniqueManager {
public:
    TechniqueManager() = default;
    ~TechniqueManager();

    TechniqueManager(const TechniqueManager&) = delete;
    TechniqueManager& operator=(const TechniqueManager&) = delete;

    Utils::CallbackList<void(std::uint16_t id, vk::Pipeline pipeline, vk::PipelineLayout layout)> on_technique_changed; // NOLINT(misc-non-private-member-variables-in-classes)

    [[nodiscard]] std::uint16_t RegisterTechnique(VulkanEngine::Runtime::VulkanBootstrap& bootstrap,
                                              const std::vector<std::uint32_t>& vert_spv,
                                              const std::vector<std::uint32_t>& frag_spv,
                                              const VulkanEngine::StandardMeshPipeline::PipelineConfig& config = {},
                                              vk::DescriptorSetLayout* bindless_layout = nullptr,
                                              vk::DescriptorSetLayout* object_data_layout = nullptr,
                                              vk::DescriptorSetLayout* raw_vertex_layout = nullptr,
                                              vk::DescriptorSetLayout* indirection_layout = nullptr);

    [[nodiscard]] std::uint16_t RegisterTechnique(VulkanEngine::Runtime::VulkanBootstrap& bootstrap,
                                              const ShaderOverride& override,
                                              const VulkanEngine::StandardMeshPipeline::PipelineConfig& config = {},
                                              vk::DescriptorSetLayout* bindless_layout = nullptr,
                                              vk::DescriptorSetLayout* object_data_layout = nullptr,
                                              vk::DescriptorSetLayout* raw_vertex_layout = nullptr,
                                              vk::DescriptorSetLayout* indirection_layout = nullptr);

    [[nodiscard]] VulkanEngine::StandardMeshPipeline::GraphicsPipeline* GetGraphicsPipeline(std::uint16_t technique_id);

    [[nodiscard]] std::uint16_t GetTechniqueCount() const { return static_cast<std::uint16_t>(techniques_.size()); }

    void Shutdown();

private:
    struct Technique {
        std::unique_ptr<VulkanEngine::StandardMeshPipeline::GraphicsPipeline> graphics_pipeline;
    };

    std::vector<Technique> techniques_{};
    std::vector<std::uint32_t> default_vert_spv_{};
    std::vector<std::uint32_t> default_frag_spv_{};
    bool has_defaults_ = false;
};

}

