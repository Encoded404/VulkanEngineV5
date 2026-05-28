module;

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>

#include <ktx.h>
#include <vulkan/vulkan_raii.hpp>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <FileLoader/Types.hpp>

module VulkanEngine.FileLoaders.TextureLoaders;

namespace VulkanEngine::FileLoaders::Textures {

namespace {

constexpr ktx_uint32_t kGlRgb = 0x1907U;
constexpr ktx_uint32_t kGlRgba = 0x1908U;
constexpr ktx_uint32_t kGlBgra = 0x80E1U;
constexpr ktx_uint32_t kGlUnsignedByte = 0x1401U;
constexpr ktx_uint32_t kGlRgba8 = 0x8058U;
constexpr ktx_uint32_t kGlRgb8 = 0x8051U;
constexpr ktx_uint32_t kGlBgra8Ext = 0x93A1U;

struct KtxTextureDeleter {
    void operator()(ktxTexture* texture) const noexcept {
        if (texture != nullptr) {
            ktxTexture_Destroy(texture);
        }
    }
};

using KtxTexturePtr = std::unique_ptr<ktxTexture, KtxTextureDeleter>;

[[nodiscard]] std::string ToLowerCopy(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

[[nodiscard]] bool HasPrefix(const FileLoader::ByteBuffer& buffer, const std::byte* prefix, std::size_t prefix_size) {
    if (buffer.size() < prefix_size) {
        return false;
    }

    return std::equal(prefix, prefix + prefix_size, buffer.begin());
}

[[nodiscard]] bool CopyBaseLevelBytes(ktxTexture* texture,
                                      TextureData& out,
                                      std::size_t pixel_size,
                                      bool swizzle_bgra,
                                      bool expand_rgb,
                                      std::string* error_message) {
    ktx_size_t image_offset = 0;

    const ktx_size_t image_size = ktxTexture_GetImageSize(texture, 0);
    if (image_size == 0) {
        if (error_message != nullptr) {
            *error_message = "KTX texture reports zero-sized base level";
        }
        return false;
    }

    if (ktxTexture_GetImageOffset(texture, 0, 0, 0, &image_offset) != KTX_SUCCESS) {
        if (error_message != nullptr) {
            *error_message = "KTX texture failed to provide base-level image offset";
        }
        return false;
    }

    const auto* data = reinterpret_cast<const std::byte*>(ktxTexture_GetData(texture));
    if (data == nullptr) {
        if (error_message != nullptr) {
            *error_message = "KTX texture contains no pixel data";
        }
        return false;
    }

    const auto* src = data + image_offset;
    const auto* src_bytes = reinterpret_cast<const std::uint8_t*>(src);

    out.pixels.resize(static_cast<std::size_t>(out.width) * static_cast<std::size_t>(out.height) * 4U);

    if (pixel_size == 4U && !swizzle_bgra && !expand_rgb) {
        std::memcpy(out.pixels.data(), src, std::min<std::size_t>(out.pixels.size(), static_cast<std::size_t>(image_size)));
        return true;
    }

    const std::size_t pixel_count = static_cast<std::size_t>(out.width) * static_cast<std::size_t>(out.height);
    if (pixel_count == 0U) {
        if (error_message != nullptr) {
            *error_message = "KTX texture has zero dimensions";
        }
        return false;
    }

    if (pixel_size == 4U && swizzle_bgra) {
        for (std::size_t i = 0; i < pixel_count; ++i) {
            const std::size_t src_idx = i * 4U;
            const std::size_t dst_idx = i * 4U;
            out.pixels[dst_idx + 0] = static_cast<std::byte>(src_bytes[src_idx + 2]);
            out.pixels[dst_idx + 1] = static_cast<std::byte>(src_bytes[src_idx + 1]);
            out.pixels[dst_idx + 2] = static_cast<std::byte>(src_bytes[src_idx + 0]);
            out.pixels[dst_idx + 3] = static_cast<std::byte>(src_bytes[src_idx + 3]);
        }
        return true;
    }

    if (pixel_size == 3U && expand_rgb) {
        for (std::size_t i = 0; i < pixel_count; ++i) {
            const std::size_t src_idx = i * 3U;
            const std::size_t dst_idx = i * 4U;
            out.pixels[dst_idx + 0] = static_cast<std::byte>(src_bytes[src_idx + 0]);
            out.pixels[dst_idx + 1] = static_cast<std::byte>(src_bytes[src_idx + 1]);
            out.pixels[dst_idx + 2] = static_cast<std::byte>(src_bytes[src_idx + 2]);
            out.pixels[dst_idx + 3] = std::byte{255};
        }
        return true;
    }

    if (error_message != nullptr) {
        *error_message = "Unsupported KTX base-level pixel layout";
    }
    return false;
}

[[nodiscard]] bool LoadKtx1Texture(ktxTexture* texture, TextureData& out, std::string* error_message) {
    const auto* texture1 = reinterpret_cast<ktxTexture1*>(texture);
    if (texture1->glType != kGlUnsignedByte) {
        if (error_message != nullptr) {
            *error_message = "Unsupported KTX1 pixel type";
        }
        return false;
    }

    out.width = texture->baseWidth;
    out.height = texture->baseHeight;
    out.mip_levels = 1;
    out.layer_count = 1;
    out.face_count = 1;

    if (texture1->glFormat == kGlRgba || texture1->glInternalformat == kGlRgba8) {
        out.vk_format = vk::Format::eR8G8B8A8Unorm;
        return CopyBaseLevelBytes(texture, out, 4U, false, false, error_message);
    }

    if (texture1->glFormat == kGlBgra || texture1->glInternalformat == kGlBgra8Ext) {
        out.vk_format = vk::Format::eR8G8B8A8Unorm;
        return CopyBaseLevelBytes(texture, out, 4U, true, false, error_message);
    }

    if (texture1->glFormat == kGlRgb || texture1->glInternalformat == kGlRgb8) {
        out.vk_format = vk::Format::eR8G8B8A8Unorm;
        return CopyBaseLevelBytes(texture, out, 3U, false, true, error_message);
    }

    if (error_message != nullptr) {
        *error_message = "Unsupported KTX1 format for RGBA upload";
    }
    return false;
}

[[nodiscard]] bool LoadKtx2Texture(ktxTexture* texture, TextureData& out, std::string* error_message) {
    auto* texture2 = reinterpret_cast<ktxTexture2*>(texture);

    out.width = texture->baseWidth;
    out.height = texture->baseHeight;
    out.mip_levels = 1;
    out.layer_count = 1;
    out.face_count = 1;

    if (ktxTexture2_NeedsTranscoding(texture2) == KTX_TRUE) {
        const KTX_error_code transcode_result = ktxTexture2_TranscodeBasis(texture2, KTX_TTF_RGBA32, 0);
        if (transcode_result != KTX_SUCCESS) {
            if (error_message != nullptr) {
                *error_message = std::string("Failed to transcode KTX2 texture: ") + ktxErrorString(transcode_result);
            }
            return false;
        }
        out.vk_format = vk::Format::eR8G8B8A8Unorm;
        return CopyBaseLevelBytes(texture, out, 4U, false, false, error_message);
    }

    switch (static_cast<vk::Format>(texture2->vkFormat)) {
        case vk::Format::eR8G8B8A8Unorm:
        case vk::Format::eR8G8B8A8Srgb:
            out.vk_format = vk::Format::eR8G8B8A8Unorm;
            return CopyBaseLevelBytes(texture, out, 4U, false, false, error_message);
        case vk::Format::eB8G8R8A8Unorm:
        case vk::Format::eB8G8R8A8Srgb:
            out.vk_format = vk::Format::eR8G8B8A8Unorm;
            return CopyBaseLevelBytes(texture, out, 4U, true, false, error_message);
        case vk::Format::eR8G8B8Unorm:
        case vk::Format::eR8G8B8Srgb:
            out.vk_format = vk::Format::eR8G8B8A8Unorm;
            return CopyBaseLevelBytes(texture, out, 3U, false, true, error_message);
        default:
            if (error_message != nullptr) {
                *error_message = "Unsupported KTX2 vkFormat for RGBA upload";
            }
            return false;
    }
}

[[nodiscard]] bool LooksLikeKtx(const FileLoader::ByteBuffer& buffer) {
    static constexpr std::array<std::byte, 12> ktx1_magic = {
        std::byte{0xAB}, std::byte{'K'}, std::byte{'T'}, std::byte{'X'}, std::byte{' '}, std::byte{'1'},
        std::byte{'1'}, std::byte{0xBB}, std::byte{0x0D}, std::byte{0x0A}, std::byte{0x1A}, std::byte{0x0A}
    };
    static constexpr std::array<std::byte, 12> ktx2_magic = {
        std::byte{0xAB}, std::byte{'K'}, std::byte{'T'}, std::byte{'X'}, std::byte{' '}, std::byte{'2'},
        std::byte{'0'}, std::byte{0xBB}, std::byte{0x0D}, std::byte{0x0A}, std::byte{0x1A}, std::byte{0x0A}
    };

    return HasPrefix(buffer, ktx1_magic.data(), sizeof(ktx1_magic)) ||
           HasPrefix(buffer, ktx2_magic.data(), sizeof(ktx2_magic));
}

[[nodiscard]] bool LooksLikePng(const FileLoader::ByteBuffer& buffer) {
    static constexpr std::array<std::byte, 8> png_magic = {
        std::byte{0x89}, std::byte{'P'}, std::byte{'N'}, std::byte{'G'}, std::byte{0x0D}, std::byte{0x0A}, std::byte{0x1A}, std::byte{0x0A}
    };
    return HasPrefix(buffer, png_magic.data(), sizeof(png_magic));
}

[[nodiscard]] bool LooksLikeJpeg(const FileLoader::ByteBuffer& buffer) {
    return buffer.size() >= 3U &&
           buffer[0] == std::byte{0xFF} &&
           buffer[1] == std::byte{0xD8} &&
           buffer[2] == std::byte{0xFF};
}

[[nodiscard]] bool LoadByMagic(const std::filesystem::path& path,
                               const FileLoader::ByteBuffer& buffer,
                               TextureData& out,
                               std::string* error_message) {
    const auto lower_ext = ToLowerCopy(path.extension().string());
    if (lower_ext == ".ktx" || lower_ext == ".ktx2" || LooksLikeKtx(buffer)) {
        return LoadKtxTextureFromBuffer(path, buffer, out, error_message);
    }

    if (lower_ext == ".png" || LooksLikePng(buffer) ||
        lower_ext == ".jpg" || lower_ext == ".jpeg" || LooksLikeJpeg(buffer)) {
        return LoadStbTextureFromBuffer(path, buffer, out, error_message);
    }

    if (error_message != nullptr) {
        *error_message = "Unsupported texture format: " + path.string();
    }
    return false;
}

}  // namespace

[[nodiscard]] bool LoadKtxTextureFromBuffer(const std::filesystem::path& path,
                                            const FileLoader::ByteBuffer& buffer,
                                            TextureData& out,
                                            std::string* error_message) {
    out.Reset();

    if (buffer.empty()) {
        if (error_message != nullptr) {
            *error_message = "Empty texture buffer: " + path.string();
        }
        return false;
    }

    ktxTexture* raw_texture = nullptr;
    const auto* bytes = reinterpret_cast<const ktx_uint8_t*>(buffer.data());
    const auto size = static_cast<ktx_size_t>(buffer.size());
    const KTX_error_code create_result = ktxTexture_CreateFromMemory(bytes, size, KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &raw_texture);
    if (create_result != KTX_SUCCESS || raw_texture == nullptr) {
        if (error_message != nullptr) {
            *error_message = std::string("Failed to parse KTX texture '") + path.string() + "': " + ktxErrorString(create_result);
        }
        return false;
    }

    const KtxTexturePtr texture(raw_texture);
    out.width = texture->baseWidth;
    out.height = texture->baseHeight;

    const bool needs_transcoding = ktxTexture_NeedsTranscoding(texture.get()) == KTX_TRUE;
    if (needs_transcoding && texture->classId == ktxTexture2_c) {
        return LoadKtx2Texture(texture.get(), out, error_message);
    }

    if (texture->classId == ktxTexture1_c) {
        return LoadKtx1Texture(texture.get(), out, error_message);
    }

    if (texture->classId == ktxTexture2_c) {
        return LoadKtx2Texture(texture.get(), out, error_message);
    }

    if (error_message != nullptr) {
        *error_message = "Unknown KTX texture class";
    }
    return false;
}

[[nodiscard]] bool LoadStbTextureFromBuffer(const std::filesystem::path& path,
                                           const FileLoader::ByteBuffer& buffer,
                                           TextureData& out,
                                           std::string* error_message) {
    out.Reset();

    if (buffer.empty()) {
        if (error_message != nullptr) {
            *error_message = "Empty texture buffer: " + path.string();
        }
        return false;
    }

    int width = 0;
    int height = 0;
    int components = 0;
    auto* pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(buffer.data()),
                                         static_cast<int>(buffer.size()),
                                         &width,
                                         &height,
                                         &components,
                                         4);
    if (pixels == nullptr) {
        if (error_message != nullptr) {
            const char* reason = stbi_failure_reason();
            *error_message = std::string("Failed to decode image '") + path.string() + "'" + (reason != nullptr ? ": " + std::string(reason) : std::string{});
        }
        return false;
    }

    out.width = static_cast<uint32_t>(width);
    out.height = static_cast<uint32_t>(height);
    out.mip_levels = 1;
    out.layer_count = 1;
    out.face_count = 1;
    out.vk_format = vk::Format::eR8G8B8A8Unorm;
    out.pixels.resize(static_cast<std::size_t>(out.width) * static_cast<std::size_t>(out.height) * 4U);
    std::memcpy(out.pixels.data(), pixels, out.pixels.size());
    stbi_image_free(pixels);
    return true;
}

[[nodiscard]] bool LoadTextureFromBuffer(const std::filesystem::path& path,
                                         const FileLoader::ByteBuffer& buffer,
                                         TextureData& out,
                                         std::string* error_message) {
    const auto lower_ext = ToLowerCopy(path.extension().string());

    if (lower_ext == ".ktx" || lower_ext == ".ktx2") {
        return LoadKtxTextureFromBuffer(path, buffer, out, error_message);
    }
    if (lower_ext == ".png" || lower_ext == ".jpg" || lower_ext == ".jpeg") {
        return LoadStbTextureFromBuffer(path, buffer, out, error_message);
    }

    return LoadByMagic(path, buffer, out, error_message);
}

}  // namespace VulkanEngine::FileLoaders::Textures
