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

void MaterialManager::Initialize(VulkanEngine::GpuResources::StagingManager* staging_mgr) {
    materials_.clear();
    dirty_list_.clear();
    free_list_.clear();
    staging_mgr_ = staging_mgr;
}

void MaterialManager::Shutdown() {
    materials_.clear();
    dirty_list_.clear();
    free_list_.clear();
    staging_mgr_ = nullptr;
}

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
                std::uint32_t mask = p.entry->dirty_bindings;
                for (std::size_t bi = 0; bi < tech->GetBindingCount(); ++bi) {
                    const auto& binding = tech->GetBinding(bi);
                    if (binding.kind != TechniqueManager::BaseTechnique::BindingKind::PerMaterial) {
                        continue;
                    }
                    if (mask & 1u) {
                        auto* ba = tech->GetBlockArray(bi);
                        if (ba) {
                            staging_mgr_->RecordBufferCopy(p.slice,
                                ba->GetBlockArray(p.id.value / 256),
                                ba->EntrySize() * (static_cast<std::uint64_t>(p.id.value % 256)));
                        }
                    }
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
