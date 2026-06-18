module;

export module VulkanEngine.MaterialManager;

import std;
import std.compat;

import vulkan_hpp;

export import VulkanEngine.MaterialManager.MaterialId;
export import VulkanEngine.BindlessManager.TextureSlot;
export import VulkanEngine.TechniqueManager.TechniqueId;
import VulkanEngine.FileLoaders.TextureLoaders;
import VulkanEngine.ResourceSystem;
import VulkanEngine.ResourceSystem.TextureResource;
import VulkanEngine.BindlessManager;
import VulkanEngine.GpuResources.StagingManager;
import VulkanEngine.TechniqueManager.BaseTechnique;
import VulkanEngine.TechniqueManager;

// Include the MaterialHandle template header
#include "MaterialHandle.hpp"

export namespace VulkanEngine::MaterialManager {
    using TechniqueManager::TechniqueId;
    using BindlessManager::TextureSlot;

enum class BlendMode : std::uint8_t {
    Opaque = 0,
    Cutout,
    Transparent
};

// ── Old MaterialDefinition (kept for backward compatibility during migration) ──
struct MaterialDefinition {
    TechniqueId technique_id{0};
    TextureSlot texture_slot{0};
    BlendMode blend_mode{BlendMode::Opaque};
};

void ValidateTextureBlendMode(const VulkanEngine::FileLoaders::Textures::AlphaAnalysis& alpha,
                              BlendMode mode,
                              std::string_view texture_name);

// ── New MaterialEntry for the typed material system ──
struct MaterialEntry {
    TechniqueId technique_id{0};
    BlendMode blend_mode{BlendMode::Opaque};
    bool dirty = false;
    std::uint32_t dirty_bindings = 0;
    std::vector<std::byte> cpu_data;  // PerMaterial bindings only, flat buffer
};

class MaterialManager {
public:
    static MaterialManager& Get();
    static void Initialize(VulkanEngine::GpuResources::StagingManager* staging_mgr = nullptr);
    static void Shutdown();

    // ── Legacy API (kept for backward compatibility) ──
    [[nodiscard]] MaterialId RegisterMaterial(const MaterialDefinition& def,
                                                VulkanEngine::ResourceManager& resource_mgr,
                                                VulkanEngine::BindlessManager::BindlessManager& bindless_mgr);
    [[nodiscard]] const MaterialDefinition& GetMaterial(MaterialId id) const;
    void UpdateMaterialTextureSlot(MaterialId id, TextureSlot slot);
    void UpdateMaterialTechnique(MaterialId id, TechniqueId tech_id);
    [[nodiscard]] std::uint16_t GetMaterialCount() const { return static_cast<std::uint16_t>(legacy_materials_.size()); }

    // ── New typed API ──

    // Typed registration — technique type inferred from template.
    // Only PerMaterial binding data is passed; Shared data lives on the technique.
    template<typename Tech, typename... Ts>
    MaterialHandle<Tech> Register(BlendMode blend, const Ts&... data) {
        static_assert(std::is_trivially_copyable_v<Ts> && ...,
                      "All material data types must be trivially copyable (GPU POD)");

        // Validate technique exists and get its ID
        assert(technique_mgr_ != nullptr && "TechniqueManager not set — call SetTechniqueManager first");
        TechniqueId tech_id = technique_mgr_->template GetId<Tech>();
        auto* tech_ptr = technique_mgr_->GetTechnique(tech_id);
        assert(tech_ptr != nullptr && "Technique not registered for this type");

        // Count PerMaterial bindings for argument validation
        std::size_t per_material_count = 0;
        for (std::size_t bi = 0; bi < tech_ptr->GetBindingCount(); ++bi) {
            if (tech_ptr->GetBinding(bi).kind == TechniqueManager::BaseTechnique::BindingKind::PerMaterial)
                ++per_material_count;
        }
        assert(sizeof...(Ts) == per_material_count &&
               "Material data arg count must match PerMaterial binding count");

        // Validate types match binding order
        std::size_t idx = 0;
        auto check_type = [&]<typename U>(const U&) {
            assert(Tech::template HasBinding<U>() &&
                   "Material data type not declared as PerMaterial by this technique");
            assert(Tech::template GetOffset<U>() == idx &&
                   "Material data types in wrong order");
            idx += sizeof(U);
        };
        (check_type(data), ...);

        // ── Allocate material ID ──
        MaterialId id;
        if (!free_list_.empty()) {
            id = free_list_.back();
            free_list_.pop_back();
        } else {
            id = MaterialId{static_cast<std::uint16_t>(materials_.size())};
            materials_.emplace_back();
        }

        // ── Serialize PerMaterial binding data into flat cpu_data buffer ──
        auto entry = std::make_unique<MaterialEntry>();
        entry->technique_id = tech_id;
        entry->blend_mode = blend;
        entry->cpu_data.clear();
        auto write_one = [&]<typename U>(const U& d) {
            const auto* bytes = reinterpret_cast<const std::byte*>(&d);
            entry->cpu_data.insert(entry->cpu_data.end(), bytes, bytes + sizeof(U));
        };
        (write_one(data), ...);

        // ── Immediate first upload via staging → device-local ──
        if (staging_mgr_ && !entry->cpu_data.empty()) {
            std::size_t total_size = entry->cpu_data.size();
            auto staging_slice = staging_mgr_->Allocate(static_cast<std::uint64_t>(total_size), 256);
            std::memcpy(staging_slice.data, entry->cpu_data.data(), total_size);

            std::size_t upload_offset = 0;
            for (std::size_t bi = 0; bi < tech_ptr->GetBindingCount(); ++bi) {
                const auto& binding = tech_ptr->GetBinding(bi);
                if (binding.kind != TechniqueManager::BaseTechnique::BindingKind::PerMaterial) continue;
                auto* ba = tech_ptr->GetBlockArray(bi);
                if (ba) {
                    staging_mgr_->RecordBufferCopy(staging_slice,
                                                   ba->GetBlockArray(id.value / 256),
                                                   ba->EntrySize() * (static_cast<std::uint64_t>(id.value % 256)),
                                                   upload_offset,
                                                   binding.stride);
                }
                upload_offset += binding.stride;
            }

            // Flush staging so the upload takes effect immediately
            staging_mgr_->Flush();
        }

        MaterialEntry* entry_ptr = entry.get();
        materials_[id.value] = std::move(entry);

        return MaterialHandle<Tech>(id.value, entry_ptr,
                                    [this](std::uint32_t mid) { MarkDirty(MaterialId{static_cast<std::uint16_t>(mid)}); });
    }

    // ── Batched GPU upload — called once per frame ──
    void FlushDirtyMaterials();

    // ── Material lifecycle ──
    void Destroy(MaterialId id);

    // ── Called by MaterialHandle::modify() ──
    void MarkDirty(MaterialId id);

    // ── Read-only access to any material (type-erased path) ──
    template<typename T>
    const T& Get(MaterialId id) const {
        auto& entry = materials_[id.value];
        return *reinterpret_cast<const T*>(entry->cpu_data.data());
    }

    // ── Set technique manager for typed registration ──
    void SetTechniqueManager(VulkanEngine::TechniqueManager::TechniqueManager* mgr) { technique_mgr_ = mgr; }

    MaterialManager(const MaterialManager&) = delete;
    MaterialManager& operator=(const MaterialManager&) = delete;

private:
    MaterialManager() = default;
    ~MaterialManager() = default;

    // ── Legacy storage ──
    struct LegacyMaterialEntry {
        MaterialDefinition def{};
    };
    std::vector<LegacyMaterialEntry> legacy_materials_{};

    // ── New storage (pointer stability via unique_ptr) ──
    std::vector<std::unique_ptr<MaterialEntry>> materials_{};
    std::vector<MaterialId> dirty_list_{};
    std::vector<MaterialId> free_list_{};

    VulkanEngine::GpuResources::StagingManager* staging_mgr_ = nullptr;
    VulkanEngine::TechniqueManager::TechniqueManager* technique_mgr_ = nullptr;
};

} // namespace VulkanEngine::MaterialManager
