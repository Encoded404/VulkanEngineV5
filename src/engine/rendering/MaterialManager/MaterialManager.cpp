module;

#include <logging/logging_macros.hpp>

module VulkanEngine.MaterialManager;

import std;
import std.compat;

import logiface;

import VulkanEngine.GpuResources.StagingManager;
import VulkanEngine.TechniqueManager.BaseTechnique;
import VulkanEngine.TechniqueManager;

namespace VulkanEngine::MaterialManager {

MaterialManager& MaterialManager::Get() {
    static MaterialManager instance;
    return instance;
}

void MaterialManager::Initialize(VulkanEngine::GpuResources::StagingManager* staging_mgr) {
    auto& inst = Get();
    inst.legacy_materials_.clear();
    inst.materials_.clear();
    inst.dirty_list_.clear();
    inst.free_list_.clear();
    inst.staging_mgr_ = staging_mgr;
}

void MaterialManager::Shutdown() {
    auto& inst = Get();
    inst.legacy_materials_.clear();
    inst.materials_.clear();
    inst.dirty_list_.clear();
    inst.free_list_.clear();
    inst.staging_mgr_ = nullptr;
}

// ── Legacy API ──

MaterialId MaterialManager::RegisterMaterial(const MaterialDefinition& def,
                                              VulkanEngine::ResourceManager& resource_mgr,
                                              VulkanEngine::BindlessManager::BindlessManager& bindless_mgr) {
    const auto id = static_cast<uint16_t>(legacy_materials_.size());
    legacy_materials_.push_back(LegacyMaterialEntry{def});
    LOGIFACE_LOG(debug, "Registered material ID " + std::to_string(id) + ": technique_id=" +
                std::to_string(def.texture_slot.value) + " blend_mode=" + std::string(
                    static_cast<std::size_t>(def.blend_mode) == 0 ? std::string_view{"Opaque"} :
                    static_cast<std::size_t>(def.blend_mode) == 1 ? std::string_view{"Cutout"} :
                    static_cast<std::size_t>(def.blend_mode) == 2 ? std::string_view{"Transparent"} :
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
    return legacy_materials_[id.value].def;
}

void MaterialManager::UpdateMaterialTextureSlot(MaterialId id, TextureSlot slot) {
    legacy_materials_[id.value].def.texture_slot = slot;
}

void MaterialManager::UpdateMaterialTechnique(MaterialId id, TechniqueId tech_id) {
    legacy_materials_[id.value].def.technique_id = tech_id;
}

// ── New API ──

void MaterialManager::MarkDirty(MaterialId id) {
    if (id.value >= materials_.size()) return;
    auto& entry = materials_[id.value];
    if (entry && !entry->dirty) {
        entry->dirty = true;
        dirty_list_.push_back(id);
    }
}

void MaterialManager::Destroy(MaterialId id) {
    if (id.value >= materials_.size()) return;
    materials_[id.value].reset();
    free_list_.push_back(id);
}

void MaterialManager::FlushDirtyMaterials() {
    if (dirty_list_.empty()) return;  // ← common case: zero work
    if (!staging_mgr_) return;

    // Phase 1: allocate staging for all dirty materials
    struct PendingUpload {
        MaterialId id;
        MaterialEntry* entry;
        VulkanEngine::GpuResources::StagingSlice slice;
    };
    std::vector<PendingUpload> pending;
    pending.reserve(dirty_list_.size());

    for (MaterialId id : dirty_list_) {
        if (id.value >= materials_.size()) continue;
        auto& entry = materials_[id.value];
        if (!entry || !entry->dirty) continue;

        auto slice = staging_mgr_->Allocate(
            static_cast<std::uint64_t>(entry->cpu_data.size()), 256);
        std::memcpy(slice.data, entry->cpu_data.data(), entry->cpu_data.size());
        pending.push_back({id, entry.get(), slice});
    }

    // Phase 2: record per-binding buffer copies — only for dirty bindings
    // We need the technique manager to look up binding info
    for (auto& p : pending) {
        // Note: In full implementation, we'd look up the technique from p.entry->technique_id
        // and iterate bindings. For now, we just flush the full cpu_data.
        // The technique lookup requires TechniqueManager which is set via SetTechniqueManager.
        if (technique_mgr_) {
            auto* tech = technique_mgr_->GetTechnique(p.entry->technique_id);
            if (tech) {
                std::size_t src_offset = 0;
                std::uint32_t mask = p.entry->dirty_bindings;
                for (std::size_t bi = 0; bi < tech->GetBindingCount(); ++bi) {
                    const auto& binding = tech->GetBinding(bi);
                    if (binding.kind != TechniqueManager::BaseTechnique::BindingKind::PerMaterial) {
                        src_offset += binding.stride;
                        continue;
                    }
                    if (mask & 1u) {
                        auto* ba = tech->GetBlockArray(bi);
                        if (ba) {
                            staging_mgr_->RecordBufferCopy(p.slice,
                                ba->GetBlockArray(p.id.value / 256),
                                ba->EntrySize() * (static_cast<std::uint64_t>(p.id.value % 256)),
                                src_offset,
                                binding.stride);
                        }
                    }
                    src_offset += binding.stride;
                    mask >>= 1;
                }
            }
        }
        p.entry->dirty = false;
        p.entry->dirty_bindings = 0;
    }

    staging_mgr_->Flush();
    dirty_list_.clear();
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
