module;

#include <memory>

export module VulkanEngine.DefaultResources;

export import VulkanEngine.ResourceSystem;
export import VulkanEngine.ResourceSystem.TextureResource;

export namespace VulkanEngine::DefaultResources {

class DefaultResources {
public:
    [[nodiscard]] static std::shared_ptr<VulkanEngine::TextureResource> CreateCheckerboard(
        VulkanEngine::ResourceManager& manager,
        const VulkanEngine::CheckerboardConfig& config = {});

    [[nodiscard]] static std::shared_ptr<VulkanEngine::TextureResource> CreateWhiteTexture(
        VulkanEngine::ResourceManager& manager);
};

} // namespace VulkanEngine::DefaultResources
