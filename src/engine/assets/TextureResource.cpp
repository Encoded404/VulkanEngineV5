module;

#include <logging/logging_macros.hpp>

module VulkanEngine.ResourceSystem.TextureResource;

import std;

import FileLoader.Types;
import logiface;

import vulkan_hpp;

import VulkanEngine.FileLoaders.TextureLoaders;

namespace VulkanEngine {

TextureResource::TextureResource(ResourceId id)
    : Resource(std::move(id)) {
    Reset();
}

TextureResource::TextureResource(ResourceId id, std::uint32_t width, std::uint32_t height, vk::Format format,
                                  std::vector<std::byte> pixels)
    : Resource(std::move(id)), width_(width), height_(height), mip_levels_(1),
      layer_count_(1), face_count_(1), vk_format_(format), pixels_(std::move(pixels)) {
    loaded_ = !pixels_.empty();
    if (loaded_) {
        alpha_analysis_ = VulkanEngine::FileLoaders::Textures::AnalyzeAlpha(pixels_);
    }
}

uint32_t TextureResource::GetWidth() const noexcept { return width_; }
uint32_t TextureResource::GetHeight() const noexcept { return height_; }
uint32_t TextureResource::GetMipLevels() const noexcept { return mip_levels_; }
uint32_t TextureResource::GetLayerCount() const noexcept { return layer_count_; }
uint32_t TextureResource::GetFaceCount() const noexcept { return face_count_; }
vk::Format TextureResource::GetVkFormat() const noexcept { return vk_format_; }
const std::vector<std::byte>& TextureResource::GetPixels() const noexcept { return pixels_; }
bool TextureResource::HasPixels() const noexcept { return !pixels_.empty(); }
const VulkanEngine::FileLoaders::Textures::AlphaAnalysis& TextureResource::GetAlphaAnalysis() const noexcept { return alpha_analysis_; }

bool TextureResource::DoLoad() {
    LOGIFACE_LOG(error, "TextureResource '" + GetId().value + "' cannot be loaded without file buffer data");
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
    alpha_analysis_ = {};
    pixels_.clear();
}

bool TextureResource::DoLoadFromBuffer(const FileLoader::ByteBuffer& buf) {
    Reset();

    if (buf.empty()) {
        LOGIFACE_LOG(warn, "TextureResource: empty buffer for resource '" + GetId().value + "'");
        return false;
    }

    VulkanEngine::FileLoaders::Textures::TextureData decoded{};
    std::string error_message{};

    if (!VulkanEngine::FileLoaders::Textures::LoadTextureFromBuffer(GetId().value, buf, decoded, &error_message)) {
        LOGIFACE_LOG(warn, "TextureResource: failed to load texture resource '" + GetId().value + "'" + (error_message.empty() ? std::string{} : ": " + error_message));
        return false;
    }

    width_ = decoded.width;
    height_ = decoded.height;
    mip_levels_ = decoded.mip_levels;
    layer_count_ = decoded.layer_count;
    face_count_ = decoded.face_count;
    vk_format_ = decoded.vk_format;
    alpha_analysis_ = decoded.alpha_analysis;
    pixels_ = std::move(decoded.pixels);
    transcoded_ = false;
    return true;
}

}  // namespace VulkanEngine
