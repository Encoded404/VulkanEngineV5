#pragma once

#include <FileLoader/Types.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <vulkan/vulkan.hpp>

namespace VulkanEngine::FileLoaders::Textures {

struct TextureData {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mip_levels = 1;
    uint32_t layer_count = 1;
    uint32_t face_count = 1;
    vk::Format vk_format = vk::Format::eUndefined;
    std::vector<std::byte> pixels{};

    void Reset() noexcept {
        width = 0;
        height = 0;
        mip_levels = 1;
        layer_count = 1;
        face_count = 1;
        vk_format = vk::Format::eUndefined;
        pixels.clear();
    }
};

[[nodiscard]] bool LoadKtxTextureFromBuffer(const std::filesystem::path& path,
                                            const FileLoader::ByteBuffer& buffer,
                                            TextureData& out,
                                            std::string* error_message = nullptr);

[[nodiscard]] bool LoadStbTextureFromBuffer(const std::filesystem::path& path,
                                           const FileLoader::ByteBuffer& buffer,
                                           TextureData& out,
                                           std::string* error_message = nullptr);

[[nodiscard]] bool LoadTextureFromBuffer(const std::filesystem::path& path,
                                         const FileLoader::ByteBuffer& buffer,
                                         TextureData& out,
                                         std::string* error_message = nullptr);

}  // namespace VulkanEngine::FileLoaders::Textures

