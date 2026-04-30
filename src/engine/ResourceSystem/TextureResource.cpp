module;

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <FileLoader/Types.hpp>
#include <vulkan/vulkan.hpp>

#include <logging/logging.hpp>

#include <engine/FileLoaders/Textures/TextureLoaders.hpp>

module VulkanEngine.ResourceSystem.TextureResource;

namespace VulkanEngine {

TextureResource::TextureResource(std::string id)
    : Resource(std::move(id)) {
    Reset();
}

uint32_t TextureResource::GetWidth() const noexcept { return width_; }
uint32_t TextureResource::GetHeight() const noexcept { return height_; }
uint32_t TextureResource::GetMipLevels() const noexcept { return mip_levels_; }
uint32_t TextureResource::GetLayerCount() const noexcept { return layer_count_; }
uint32_t TextureResource::GetFaceCount() const noexcept { return face_count_; }
vk::Format TextureResource::GetVkFormat() const noexcept { return vk_format_; }
const std::vector<std::byte>& TextureResource::GetPixels() const noexcept { return pixels_; }
bool TextureResource::HasPixels() const noexcept { return !pixels_.empty(); }

bool TextureResource::DoLoad() {
    LOGIFACE_LOG(error, "TextureResource '" + GetId() + "' cannot be loaded without file buffer data");
    return false;
}

bool TextureResource::DoUnload() {
    Reset();
    return true;
}

void TextureResource::Reset() noexcept {
    width_ = 0;
    height_ = 0;
    mip_levels_ = 0;
    layer_count_ = 0;
    face_count_ = 0;
    vk_format_ = vk::Format::eUndefined;
    transcoded_ = false;
    pixels_.clear();
}

bool TextureResource::DoLoadFromBuffer(const FileLoader::ByteBuffer& buf) {
    Reset();

    if (buf.empty()) {
        LOGIFACE_LOG(warn, "TextureResource: empty buffer for resource '" + GetId() + "'");
        return false;
    }

    VulkanEngine::FileLoaders::Textures::TextureData decoded{};
    std::string error_message{};

    if (!VulkanEngine::FileLoaders::Textures::LoadTextureFromBuffer(GetId(), buf, decoded, &error_message)) {
        LOGIFACE_LOG(warn, "TextureResource: failed to load texture resource '" + GetId() + "'" + (error_message.empty() ? std::string{} : ": " + error_message));
        return false;
    }

    width_ = decoded.width;
    height_ = decoded.height;
    mip_levels_ = decoded.mip_levels;
    layer_count_ = decoded.layer_count;
    face_count_ = decoded.face_count;
    vk_format_ = decoded.vk_format;
    pixels_ = std::move(decoded.pixels);
    transcoded_ = false;
    return true;
}

std::shared_ptr<TextureResource> TextureResource::CreateCheckerboardTexture(std::string id, const CheckerboardConfig& config) {
    auto res = std::make_shared<TextureResource>(std::move(id));
    res->width_ = config.size;
    res->height_ = config.size;
    res->mip_levels_ = 1;
    res->layer_count_ = 1;
    res->face_count_ = 1;
    res->vk_format_ = vk::Format::eR8G8B8A8Unorm;
    res->transcoded_ = false;

    res->pixels_.resize(static_cast<std::size_t>(config.size) * config.size * 4);
    const uint32_t square_size = config.size / config.squares;

    for (uint32_t y = 0; y < config.size; ++y) {
        for (uint32_t x = 0; x < config.size; ++x) {
            const bool is_color1 = (((x / square_size) + (y / square_size)) % 2U) == 0U;
            const auto& color = is_color1 ? config.color1 : config.color2;
            const size_t idx = (static_cast<size_t>(y) * config.size + x) * 4;
            res->pixels_[idx + 0] = static_cast<std::byte>(color[0]);
            res->pixels_[idx + 1] = static_cast<std::byte>(color[1]);
            res->pixels_[idx + 2] = static_cast<std::byte>(color[2]);
            res->pixels_[idx + 3] = static_cast<std::byte>(color[3]);
        }
    }

    res->loaded_ = true;
    return res;
}

}  // namespace VulkanEngine
