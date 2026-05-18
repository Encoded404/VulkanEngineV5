module;

#include <cstdint>
#include <vector>

#include <glm/glm.hpp> //NOLINT(misc-include-cleaner)
#include <vulkan/vulkan_raii.hpp>

export module VulkanEngine.MaterialManager;

export import VulkanBackend.Runtime.VulkanBootstrap;
export import VulkanEngine.StandardMeshPipeline;
export import VulkanEngine.ResourceSystem;
export import VulkanEngine.ResourceSystem.TextureResource;
export import VulkanEngine.GpuResources;
export import VulkanEngine.TechniqueManager;
export import VulkanEngine.BindlessManager;

export namespace VulkanEngine::MaterialManager {

struct MaterialDefinition {
    uint16_t technique_id = 0;
    uint32_t bindless_texture_slot = 0;  // Index into BindlessManager's texture array
    glm::vec4 base_color_factor{1.0f, 1.0f, 1.0f, 1.0f};
    float metallic_factor = 0.0f;
    float roughness_factor = 1.0f;
};

class MaterialManager {
public:
    MaterialManager() = default;
    ~MaterialManager();

    MaterialManager(const MaterialManager&) = delete;
    MaterialManager& operator=(const MaterialManager&) = delete;

    void Initialize(VulkanEngine::Runtime::VulkanBootstrap& bootstrap,
                    VulkanEngine::TechniqueManager::TechniqueManager& technique_manager,
                    VulkanEngine::BindlessManager::BindlessManager& bindless_manager);

    [[nodiscard]] uint16_t RegisterMaterial(const MaterialDefinition& def);

    [[nodiscard]] const MaterialDefinition& GetMaterial(uint16_t id) const;
    [[nodiscard]] uint16_t GetMaterialCount() const { return static_cast<uint16_t>(materials_.size()); }

    void Shutdown();

private:
    struct MaterialEntry {
        MaterialDefinition def{};
    };

    VulkanEngine::Runtime::VulkanBootstrap* bootstrap_ = nullptr;
    VulkanEngine::TechniqueManager::TechniqueManager* technique_manager_ = nullptr;
    VulkanEngine::BindlessManager::BindlessManager* bindless_manager_ = nullptr;
    std::vector<MaterialEntry> materials_{};
};

}
