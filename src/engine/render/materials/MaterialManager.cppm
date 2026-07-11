module;

#include <cassert>

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

// Import the MaterialHandle template from the module partition
export import :MaterialHandle;

export namespace VulkanEngine::MaterialManager {
    using TechniqueManager::TechniqueId;

void ValidateTextureBlendMode(const VulkanEngine::FileLoaders::Textures::AlphaAnalysis& alpha,
                              BlendMode mode,
                              std::string_view texture_name);

class MaterialManager {
public:
    void Initialize(VulkanEngine::GpuResources::StagingManager* staging_mgr = nullptr);
    void Shutdown();

    // Typed registration — technique type inferred from template.
    // Only PerMaterial binding data is passed; Shared data lives on the technique.
    template<typename Tech, typename... Ts>
    MaterialHandle<Tech> Register(BlendMode blend, const Ts&... data) {
        static_assert((std::is_trivially_copyable_v<Ts> && ...),
                      "All material data types must be trivially copyable (GPU POD)");

        // Validate technique exists and get its ID
        assert(technique_mgr != nullptr && "TechniqueManager not set — call SetTechniqueManager first");
        const TechniqueId tech_id = technique_mgr->template GetId<Tech>();
        auto* tech_ptr = technique_mgr->GetTechnique(tech_id);
        assert(tech_ptr != nullptr && "Technique not registered for this type");

        // ── Allocate material ID ──
        MaterialId id;
        if (!Free_list.empty()) {
            id = Free_list.back();
            Free_list.pop_back();
        } else {
            id = MaterialId{static_cast<std::uint32_t>(Materials.size())};
            Materials.emplace_back();
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
        if (staging_mgr && !entry->cpu_data.empty()) {
            const std::size_t total_size = entry->cpu_data.size();
            const GpuResources::StagingSlice staging_slice = staging_mgr->Allocate(static_cast<std::uint64_t>(total_size), 256);
            std::memcpy(staging_slice.data, entry->cpu_data.data(), total_size);

            for (std::size_t bi = 0; bi < tech_ptr->GetBindingCount(); ++bi) {
                if (const auto& binding = tech_ptr->GetBinding(bi); binding.kind != TechniqueManager::BaseTechnique::BindingKind::PerMaterial) continue;
                if (const GpuResources::BlockArray* ba = tech_ptr->GetBlockArray(bi)) {
                    staging_mgr->RecordBufferCopy(staging_slice,
                                                   ba->GetBlockArray(id.value / 256),
                                                   ba->EntrySize() * (static_cast<std::uint64_t>(id.value % 256)));
                }
            }

            staging_mgr->Flush();
        }

        MaterialEntry* entry_ptr = entry.get();
        Materials[id.value] = std::move(entry);

        return MaterialHandle<Tech>(id.value, entry_ptr,
            [this](const std::uint32_t mid) { MarkDirty(MaterialId{mid}); });
    }

    // ── Batched GPU upload — called once per frame ──
    void FlushDirtyMaterials();

    // ── Material lifecycle ──
    void Destroy(MaterialId id);

    // ── Called by MaterialHandle::modify() ──
    void MarkDirty(MaterialId id);

    // ── Read-only access to any material (type-erased path) ──
    template<typename T>
    const T& Get(const MaterialId id) const {
        auto& entry = Materials[id.value];
        return *reinterpret_cast<const T*>(entry->cpu_data.data());
    }

    // ── Set technique manager for typed registration ──
    void SetTechniqueManager(VulkanEngine::TechniqueManager::TechniqueManager* mgr) { technique_mgr = mgr; }

    // ── Type-erased access to raw material data for PackMaterialData() ──
    [[nodiscard]] const void* GetRawData(MaterialId id) const {
        return Materials[id.value]->cpu_data.data();
    }

    // ── Look up the BaseTechnique for a material (via stored technique_id) ──
    [[nodiscard]] TechniqueManager::BaseTechnique* GetTechniqueForMaterial(MaterialId id) const {
        auto& entry = Materials[id.value];
        return technique_mgr->GetTechnique(entry->technique_id);
    }

    MaterialManager(const MaterialManager&) = delete;
    MaterialManager& operator=(const MaterialManager&) = delete;

    // Default constructible — owned by EngineContext
    MaterialManager() = default;
    ~MaterialManager() = default;

    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    // New typed storage (pointer stability via unique_ptr)
    std::vector<std::unique_ptr<MaterialEntry>> Materials{};
    std::vector<MaterialId> Dirty_list{};
    std::vector<MaterialId> Free_list{};
    // NOLINTEND(misc-non-private-member-variables-in-classes)

    GpuResources::StagingManager* staging_mgr = nullptr; // NOLINT(misc-non-private-member-variables-in-classes)
    TechniqueManager::TechniqueManager* technique_mgr = nullptr; // NOLINT(misc-non-private-member-variables-in-classes)
};

} // namespace VulkanEngine::MaterialManager
