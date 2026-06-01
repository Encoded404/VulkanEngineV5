module;

#include <memory>

export module VulkanEngine.DefaultTextureFactory;

export import VulkanEngine.ResourceSystem;
export import VulkanEngine.ResourceSystem.TextureResource;

export namespace VulkanEngine::DefaultTextureFactory {

class DefaultTextureFactory {
public:
    [[nodiscard]] static std::shared_ptr<VulkanEngine::TextureResource> CreateCheckerboard(
        VulkanEngine::ResourceManager& manager,
        const VulkanEngine::CheckerboardConfig& config = {});

    [[nodiscard]] static std::shared_ptr<VulkanEngine::TextureResource> CreateWhiteTexture(
        VulkanEngine::ResourceManager& manager);

    [[nodiscard]] static std::shared_ptr<VulkanEngine::TextureResource> CreateSolidColorTexture(
        VulkanEngine::ResourceManager& manager,
        const std::array<uint8_t, 4>& color);
};

} // namespace VulkanEngine::DefaultTextureFactory
