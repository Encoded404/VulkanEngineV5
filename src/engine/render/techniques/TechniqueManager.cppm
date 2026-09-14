module;

export module VulkanEngine.TechniqueManager;

import std;
import std.compat;

import vulkan_hpp;

import VulkanEngine.ShaderManager;
import VulkanEngine.PipelineFactory;

export import VulkanBackend.Vulkan.VulkanBootstrap;
export import VulkanShared.CallbackList;
export import VulkanEngine.StandardMeshPipeline;
export import VulkanEngine.TechniqueManager.BaseTechnique;
export import VulkanEngine.TechniqueManager.DefaultMeshTechnique;
export import VulkanEngine.TechniqueManager.UnlitTextureTechnique;

#ifndef UINT16_MAX
constexpr std::uint16_t UINT16_MAX =
    std::numeric_limits<std::uint16_t>::max();
#endif

export namespace VulkanEngine::TechniqueManager {

// TechniquePacking lives in the BaseTechnique module (exported below via the
// BaseTechnique re-export) so BaseTechnique::PackMaterialData can share it.

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

    // ── Hot-reload: rebuild technique pipelines whose shaders changed ──
    void PollShaders(ShaderSystem::ShaderManager& shaders,
                     ShaderSystem::PipelineFactory& factory,
                     std::uint32_t frame_index);

private:
    friend class BaseTechnique;

    struct Technique {
        std::unique_ptr<BaseTechnique> base_technique;
    };

    std::vector<Technique> techniques_{};
    std::unordered_map<std::type_index, TechniqueId> type_to_id_{};
};

}

