module;

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <FileLoader/Types.hpp>
#include <ktx.h>
#include <vulkan/vulkan.hpp>

#include <logging/logging.hpp>

module VulkanEngine.ResourceSystem.TextureResource;

namespace VulkanEngine {

namespace {

struct KtxTextureDeleter {
    void operator()(ktxTexture* texture) const noexcept {
        if (texture != nullptr) {
            ktxTexture_Destroy(texture);
        }
    }
};

using KtxTexturePtr = std::unique_ptr<ktxTexture, KtxTextureDeleter>;

[[nodiscard]] const char* ErrorString(KTX_error_code code) {
    const char* text = ktxErrorString(code);
    return text != nullptr ? text : "unknown KTX error";
}

}  // namespace

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

    ktxTexture2* raw_texture = nullptr;
    const auto* bytes = reinterpret_cast<const ktx_uint8_t*>(buf.data());
    const auto size = static_cast<ktx_size_t>(buf.size());
    const KTX_error_code create_result = ktxTexture2_CreateFromMemory(
        bytes,
        size,
        KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,
        &raw_texture);

    if (create_result != KTX_SUCCESS || raw_texture == nullptr) {
        LOGIFACE_LOG(warn, "TextureResource: failed to parse KTX2 resource '" + GetId() + "' (" + ErrorString(create_result) + ")");
        return false;
    }

    const KtxTexturePtr texture(reinterpret_cast<ktxTexture*>(raw_texture));
    auto* texture2 = reinterpret_cast<ktxTexture2*>(texture.get());

    if (ktxTexture2_NeedsTranscoding(texture2) == KTX_TRUE) {
        const KTX_error_code transcode_result = ktxTexture2_TranscodeBasis(texture2, KTX_TTF_RGBA32, 0);
        if (transcode_result != KTX_SUCCESS) {
            LOGIFACE_LOG(warn, "TextureResource: failed to transcode KTX2 resource '" + GetId() + "' (" + ErrorString(transcode_result) + ")");
            return false;
        }
        transcoded_ = true;
        vk_format_ = vk::Format::eR8G8B8A8Unorm;
    } else {
        vk_format_ = static_cast<vk::Format>(texture2->vkFormat);
        if (vk_format_ != vk::Format::eR8G8B8A8Unorm &&
            vk_format_ != vk::Format::eB8G8R8A8Unorm) {
            LOGIFACE_LOG(warn, "TextureResource: unsupported KTX2 vkFormat=" + std::to_string(texture2->vkFormat) + " for resource '" + GetId() + "'");
            return false;
        }
    }

    width_ = texture2->baseWidth;
    height_ = texture2->baseHeight;
    mip_levels_ = std::max(1u, texture2->numLevels);
    layer_count_ = std::max(1u, texture2->numLayers);
    face_count_ = std::max(1u, texture2->numFaces);

    const ktx_size_t data_size = transcoded_ ? ktxTexture_GetDataSizeUncompressed(texture.get())
                                             : ktxTexture_GetDataSize(texture.get());
    const auto* data_ptr = reinterpret_cast<const std::byte*>(ktxTexture_GetData(texture.get()));

    if (data_ptr == nullptr || data_size == 0) {
        LOGIFACE_LOG(warn, "TextureResource: no pixel data found for resource '" + GetId() + "'");
        return false;
    }

    pixels_.resize(static_cast<std::size_t>(data_size));
    std::memcpy(pixels_.data(), data_ptr, static_cast<std::size_t>(data_size));
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
