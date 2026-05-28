module;

#include <cstdint>
#include <vector>

module VulkanEngine.MaterialManager;

namespace VulkanEngine::MaterialManager {

MaterialManager& MaterialManager::Get() {
    static MaterialManager instance;
    return instance;
}

void MaterialManager::Initialize() {
    auto& inst = Get();
    inst.materials_.clear();
}

void MaterialManager::Shutdown() {
    auto& inst = Get();
    inst.materials_.clear();
}

MaterialId MaterialManager::RegisterMaterial(const MaterialDefinition& def) {
    const auto id = static_cast<uint16_t>(materials_.size());
    materials_.push_back(MaterialEntry{def});
    LOGIFACE_LOG(debug, "Registered material ID " + std::to_string(id) + ": technique_id=" +
                 std::to_string(def.technique_id.value) + " texture_slot=" +
                 std::to_string(def.texture_slot.value));
    return MaterialId{id};
}

const MaterialDefinition& MaterialManager::GetMaterial(MaterialId id) const {
    return materials_[id.value].def;
}

void MaterialManager::UpdateMaterialTextureSlot(MaterialId id, TextureSlot slot) {
    materials_[id.value].def.texture_slot = slot;
}

void MaterialManager::UpdateMaterialTechnique(MaterialId id, TechniqueId tech_id) {
    materials_[id.value].def.technique_id = tech_id;
}

} // namespace VulkanEngine::MaterialManager
