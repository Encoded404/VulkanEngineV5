module;

#include <cstdint>
#include <vector>

#include <vulkan/vulkan_raii.hpp>

export module VulkanEngine.MaterialManager;

export import VulkanEngine.MaterialId;
export import VulkanEngine.TextureSlot;
export import VulkanEngine.TechniqueId;

export namespace VulkanEngine::MaterialManager {

struct MaterialDefinition {
    TechniqueId technique_id{0};
    TextureSlot texture_slot{0};
};

class MaterialManager {
public:
    static MaterialManager& Get();

    static void Initialize();
    static void Shutdown();

    [[nodiscard]] MaterialId RegisterMaterial(const MaterialDefinition& def);

    [[nodiscard]] const MaterialDefinition& GetMaterial(MaterialId id) const;
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
