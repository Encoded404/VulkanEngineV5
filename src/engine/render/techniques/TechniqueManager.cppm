module;

export module VulkanEngine.TechniqueManager;

import std;
import std.compat;

import vulkan_hpp;

export import VulkanBackend.Vulkan.VulkanBootstrap;
export import VulkanShared.CallbackList;
export import VulkanEngine.StandardMeshPipeline;
export import VulkanEngine.TechniqueManager.BaseTechnique;
export import VulkanEngine.TechniqueManager.DefaultMeshTechnique;

#ifndef UINT16_MAX
constexpr std::uint16_t UINT16_MAX =
    std::numeric_limits<std::uint16_t>::max();
#endif

export namespace VulkanEngine::TechniqueManager {

// ── Configurable bit packing for StaticEntry.technique_material ──
// Change TECHNIQUE_BITS to redistribute bits between material_id and technique_id.
// Keep in sync with kTechniqueBits in expand.slang.
namespace TechniquePacking {
    inline constexpr uint32_t TECHNIQUE_BITS  = 12;              // → 4096 techniques max
    inline constexpr uint32_t MATERIAL_BITS   = 32 - TECHNIQUE_BITS;  // → 1M materials max
    inline constexpr uint32_t TECHNIQUE_MASK  = (1u << TECHNIQUE_BITS) - 1;

    constexpr uint32_t Pack(uint32_t material_id, uint32_t technique_id) {
        return (material_id << TECHNIQUE_BITS) | (technique_id & TECHNIQUE_MASK);
    }
    constexpr uint32_t UnpackTechnique(uint32_t packed) {
        return packed & TECHNIQUE_MASK;
    }
    constexpr uint32_t UnpackMaterial(uint32_t packed) {
        return packed >> TECHNIQUE_BITS;
    }
}

class TechniqueManager {
public:
    TechniqueManager() = default;
    ~TechniqueManager();

    TechniqueManager(const TechniqueManager&) = delete;
    TechniqueManager& operator=(const TechniqueManager&) = delete;

    VulkanShared::CallbackList<void(std::uint16_t id, vk::Pipeline pipeline, vk::PipelineLayout layout)> on_technique_changed; // NOLINT(misc-non-private-member-variables-in-classes)

    // Get technique by ID
    [[nodiscard]] BaseTechnique* GetTechnique(std::uint16_t technique_id);
    [[nodiscard]] BaseTechnique* GetTechnique(TechniqueId id);

    [[nodiscard]] std::uint16_t GetTechniqueCount() const { return static_cast<std::uint16_t>(techniques_.size()); }

    // Register a typed technique
    template<typename Tech>
        requires std::derived_from<Tech, BaseTechnique>
    TechniqueId Register(std::unique_ptr<Tech> technique) {
        auto id = TechniqueId{static_cast<std::uint16_t>(techniques_.size())};
        technique->SetId(id);
        type_to_id_[std::type_index(typeid(Tech))] = id;
        Technique t;
        t.base_technique = std::move(technique);
        techniques_.push_back(std::move(t));
        return id;
    }

    // Get technique ID by type
    template<typename Tech>
    [[nodiscard]] TechniqueId GetId() const {
        if (const auto it = type_to_id_.find(std::type_index(typeid(Tech))); it != type_to_id_.end()) return it->second;
        return TechniqueId{UINT16_MAX};
    }

    void Shutdown();

private:
    friend class BaseTechnique;

    struct Technique {
        std::unique_ptr<BaseTechnique> base_technique;
    };

    std::vector<Technique> techniques_{};
    std::unordered_map<std::type_index, TechniqueId> type_to_id_{};
};

}

