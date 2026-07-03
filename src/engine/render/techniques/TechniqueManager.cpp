module;


module VulkanEngine.TechniqueManager;

import std;
import std.compat;

import vulkan_hpp;

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

}

