module;

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <logging/logging.hpp>

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

MaterialId MaterialManager::RegisterMaterial(const MaterialDefinition& def,
                                              VulkanEngine::ResourceManager& resource_mgr,
                                              VulkanEngine::BindlessManager::BindlessManager& bindless_mgr) {
    const auto id = static_cast<uint16_t>(materials_.size());
    materials_.push_back(MaterialEntry{def});
    LOGIFACE_LOG(debug, "Registered material ID " + std::to_string(id) + ": technique_id=" +
                std::to_string(def.texture_slot.value) + " blend_mode=" + std::string(
                    static_cast<size_t>(def.blend_mode) == 0 ? std::string_view{"Opaque"} :
                    static_cast<size_t>(def.blend_mode) == 1 ? std::string_view{"Cutout"} :
                    static_cast<size_t>(def.blend_mode) == 2 ? std::string_view{"Transparent"} :
                    std::string_view{"Unknown"})
               );

    const auto* rid = bindless_mgr.GetTextureId(def.texture_slot.value);
    if (rid != nullptr) {
        auto* tex = resource_mgr.GetResource<VulkanEngine::TextureResource>(*rid);
        if (tex != nullptr) {
            ValidateTextureBlendMode(tex->GetAlphaAnalysis(), def.blend_mode, rid->value);
        }
    }

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

void ValidateTextureBlendMode(const VulkanEngine::FileLoaders::Textures::AlphaAnalysis& alpha,
                               BlendMode mode,
                               std::string_view texture_name) {
    const std::string name(texture_name);

    if (mode == BlendMode::Opaque) {
        if (alpha.hasFractionalAlpha) {
            LOGIFACE_LOG(warn, "Warning: texture '" + name + "' contains fractional alpha, "
                         "recommended blend mode is Transparent but current mode is Opaque");
        }
        if (alpha.hasZeroAlpha) {
            LOGIFACE_LOG(warn, "Warning: texture '" + name + "' has zero-alpha pixels, "
                         "may render incorrectly with Opaque blend mode");
        }
    } else if (mode == BlendMode::Cutout) {
        if (alpha.opaqueCoverage >= 1.0f) {
            LOGIFACE_LOG(warn, "Warning: texture '" + name + "' is fully opaque, "
                         "Cutout blend mode has no effect (use Opaque)");
        }
        if (alpha.hasFractionalAlpha) {
            LOGIFACE_LOG(warn, "Warning: texture '" + name + "' contains fractional alpha, "
                         "recommended blend mode is Transparent but current mode is Cutout");
        }
    } else if (mode == BlendMode::Transparent) {
        if (alpha.opaqueCoverage >= 1.0f) {
            LOGIFACE_LOG(warn, "Warning: texture '" + name + "' is fully opaque, "
                         "Transparent blend mode is unnecessary (use Opaque)");
        }
    }
}

} // namespace VulkanEngine::MaterialManager
