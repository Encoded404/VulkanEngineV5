module;

#include <cstddef>
#include <memory>
#include <vector>

module VulkanEngine.DefaultTextureFactory;

import VulkanEngine.ResourceSystem;
import VulkanEngine.ResourceSystem.TextureResource;

namespace VulkanEngine::DefaultTextureFactory {

std::shared_ptr<VulkanEngine::TextureResource> DefaultTextureFactory::CreateCheckerboard(
    VulkanEngine::ResourceManager&  /*manager*/,
    const VulkanEngine::CheckerboardConfig& config) {
    const uint32_t pixel_count = config.size * config.size * 4;
    std::vector<std::byte> pixels(pixel_count);
    const uint32_t square_size = config.size / config.squares;

    for (uint32_t y = 0; y < config.size; ++y) {
        for (uint32_t x = 0; x < config.size; ++x) {
            const bool is_color1 = (((x / square_size) + (y / square_size)) % 2U) == 0U;
            const auto& color = is_color1 ? config.color1 : config.color2;
            const size_t idx = (static_cast<size_t>(y) * config.size + x) * 4;
            pixels[idx + 0] = static_cast<std::byte>(color[0]);
            pixels[idx + 1] = static_cast<std::byte>(color[1]);
            pixels[idx + 2] = static_cast<std::byte>(color[2]);
            pixels[idx + 3] = static_cast<std::byte>(color[3]);
        }
    }

    return std::make_shared<VulkanEngine::TextureResource>(
        VulkanEngine::ResourceId{"checkerboard_default"}, config.size, config.size,
        vk::Format::eR8G8B8A8Unorm, std::move(pixels));
}

std::shared_ptr<VulkanEngine::TextureResource> DefaultTextureFactory::CreateWhiteTexture(
    VulkanEngine::ResourceManager&  manager) {
    VulkanEngine::CheckerboardConfig white_config{};
    white_config.color1 = {255, 255, 255, 255};
    white_config.color2 = {255, 255, 255, 255};
    return CreateCheckerboard(manager, white_config);
}

std::shared_ptr<VulkanEngine::TextureResource> DefaultTextureFactory::CreateSolidColorTexture(
    VulkanEngine::ResourceManager&  /*manager*/,
    const std::array<uint8_t, 4>& color) {
    std::vector<std::byte> pixels(4);
    pixels[0] = static_cast<std::byte>(color[0]);
    pixels[1] = static_cast<std::byte>(color[1]);
    pixels[2] = static_cast<std::byte>(color[2]);
    pixels[3] = static_cast<std::byte>(color[3]);

    return std::make_shared<VulkanEngine::TextureResource>(
        VulkanEngine::ResourceId{"solid_color"}, 1, 1,
        vk::Format::eR8G8B8A8Unorm, std::move(pixels));
}

} // namespace VulkanEngine::DefaultTextureFactory
