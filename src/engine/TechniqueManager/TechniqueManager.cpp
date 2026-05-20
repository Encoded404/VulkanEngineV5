module;

#include <cstdint>
#include <memory>
#include <vector>

module VulkanEngine.TechniqueManager;

import VulkanEngine.StandardMeshPipeline;

namespace VulkanEngine::TechniqueManager {

TechniqueManager::~TechniqueManager() {
    Shutdown();
}

uint16_t TechniqueManager::RegisterTechnique(VulkanEngine::Runtime::VulkanBootstrap& bootstrap,
                                              const std::vector<uint32_t>& vert_spv,
                                              const std::vector<uint32_t>& frag_spv,
                                              const VulkanEngine::StandardMeshPipeline::PipelineConfig& config,
                                              vk::DescriptorSetLayout* bindless_layout,
                                              vk::DescriptorSetLayout* instance_data_layout,
                                              vk::DescriptorSetLayout* expanded_data_layout) {
    const uint16_t id = static_cast<uint16_t>(techniques_.size());

    auto pipeline_manager = std::make_unique<VulkanEngine::StandardMeshPipeline::PipelineManager>();
    pipeline_manager->Initialize(bootstrap, vert_spv, frag_spv, config, bindless_layout, instance_data_layout, expanded_data_layout);

    techniques_.push_back(Technique{std::move(pipeline_manager)});
    return id;
}

VulkanEngine::StandardMeshPipeline::PipelineManager* TechniqueManager::GetPipelineManager(uint16_t technique_id) {
    if (technique_id >= techniques_.size()) return nullptr;
    return techniques_[technique_id].pipeline_manager.get();
}

void TechniqueManager::Shutdown() {
    techniques_.clear();
}

}
