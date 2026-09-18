module;


module VulkanEngine.TechniqueManager;

import std;
import std.compat;

import vulkan_hpp;

import VulkanEngine.ShaderManager;
import VulkanEngine.PipelineFactory;

namespace VulkanEngine::TechniqueManager {

TechniqueManager::~TechniqueManager() {
    Shutdown();
}

BaseTechnique* TechniqueManager::GetTechnique(uint16_t technique_id) {
    if (technique_id >= techniques_.size()) return nullptr;
    return techniques_[technique_id].base_technique.get();
}

BaseTechnique* TechniqueManager::GetTechnique(TechniqueId id) {
    return GetTechnique(id.value);
}

void TechniqueManager::Shutdown() {
    techniques_.clear();
}

void TechniqueManager::PollShaders(ShaderSystem::ShaderManager& shaders,
                                   ShaderSystem::PipelineFactory& factory,
                                   std::uint32_t frame_index) {
    for (auto& technique : techniques_) {
        if (technique.base_technique) {
            technique.base_technique->PollAndRebuild(shaders, factory, frame_index);
        }
    }
}

}

