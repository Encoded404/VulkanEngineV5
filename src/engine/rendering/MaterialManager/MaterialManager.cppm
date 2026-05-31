module;

#include <cstdint>
#include <vector>

#include <vulkan/vulkan_raii.hpp>

export module VulkanEngine.MaterialManager;

export import VulkanEngine.MaterialManager.MaterialId;
export import VulkanEngine.BindlessManager.TextureSlot;
export import VulkanEngine.TechniqueManager.TechniqueId;
import VulkanEngine.FileLoaders.TextureLoaders;
import VulkanEngine.ResourceSystem;
import VulkanEngine.ResourceSystem.TextureResource;
import VulkanEngine.BindlessManager;

export namespace VulkanEngine::MaterialManager {
    using TechniqueManager::TechniqueId;
    using BindlessManager::TextureSlot;

enum class BlendMode : uint8_t {
    Opaque = 0,
    Cutout,
    Transparent
};

struct MaterialDefinition {
    TechniqueId technique_id{0};
    TextureSlot texture_slot{0};
    BlendMode blend_mode{BlendMode::Opaque};
};

void ValidateTextureBlendMode(const VulkanEngine::FileLoaders::Textures::AlphaAnalysis& alpha,
                              BlendMode mode,
                              std::string_view texture_name);

class MaterialManager {
public:
    static MaterialManager& Get();

    static void Initialize();
    static void Shutdown();

    [[nodiscard]] MaterialId RegisterMaterial(const MaterialDefinition& def,
                                                VulkanEngine::ResourceManager& resource_mgr,
                                                VulkanEngine::BindlessManager::BindlessManager& bindless_mgr);

    [[nodiscard]] const MaterialDefinition& GetMaterial(MaterialId id) const;
    void UpdateMaterialTextureSlot(MaterialId id, TextureSlot slot);
    void UpdateMaterialTechnique(MaterialId id, TechniqueId tech_id);
    [[nodiscard]] uint16_t GetMaterialCount() const { return static_cast<uint16_t>(materials_.size()); }

    MaterialManager(const MaterialManager&) = delete;
    MaterialManager& operator=(const MaterialManager&) = delete;

private:
    MaterialManager() = default;
    ~MaterialManager() = default;

    struct MaterialEntry {
        MaterialDefinition def{};
    };

    std::vector<MaterialEntry> materials_{};
};

} // namespace VulkanEngine::MaterialManager
