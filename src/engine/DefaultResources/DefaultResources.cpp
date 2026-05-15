module;

#include <memory>

module VulkanEngine.DefaultResources;

import VulkanEngine.ResourceSystem;
import VulkanEngine.ResourceSystem.TextureResource;

namespace VulkanEngine::DefaultResources {

std::shared_ptr<VulkanEngine::TextureResource> DefaultResources::CreateCheckerboard(
    VulkanEngine::ResourceManager& manager,
    const VulkanEngine::CheckerboardConfig& config) {
    auto texture = VulkanEngine::TextureResource::CreateCheckerboardTexture("checkerboard_default", config);
    return texture;
}

std::shared_ptr<VulkanEngine::TextureResource> DefaultResources::CreateWhiteTexture(
    VulkanEngine::ResourceManager& manager) {
    VulkanEngine::CheckerboardConfig white_config{};
    white_config.color1 = {255, 255, 255, 255};
    white_config.color2 = {255, 255, 255, 255};
    auto texture = VulkanEngine::TextureResource::CreateCheckerboardTexture("white_default", white_config);
    return texture;
}

} // namespace VulkanEngine::DefaultResources
