module;

#include <cstdint>
#include <vector>

#include <vulkan/vulkan_raii.hpp>

module VulkanEngine.MaterialManager;

import VulkanBackend.Runtime.VulkanBootstrap;
import VulkanEngine.StandardMeshPipeline;
import VulkanEngine.ResourceSystem;
import VulkanEngine.ResourceSystem.TextureResource;
import VulkanEngine.GpuResources;
import VulkanEngine.TechniqueManager;
import VulkanEngine.BindlessManager;

namespace VulkanEngine::MaterialManager {

MaterialManager::~MaterialManager() {
    Shutdown();
}

void MaterialManager::Initialize(VulkanEngine::Runtime::VulkanBootstrap& bootstrap,
                                  VulkanEngine::TechniqueManager::TechniqueManager& technique_manager,
                                  VulkanEngine::BindlessManager::BindlessManager& bindless_manager) {
    bootstrap_ = &bootstrap;
    technique_manager_ = &technique_manager;
    bindless_manager_ = &bindless_manager;
}

uint16_t MaterialManager::RegisterMaterial(const MaterialDefinition& def) {
    const uint16_t id = static_cast<uint16_t>(materials_.size());
    materials_.push_back(MaterialEntry{def});
    return id;
}

const MaterialDefinition& MaterialManager::GetMaterial(uint16_t id) const {
    return materials_[id].def;
}

void MaterialManager::Shutdown() {
    materials_.clear();
    bindless_manager_ = nullptr;
    technique_manager_ = nullptr;
    bootstrap_ = nullptr;
}

}
