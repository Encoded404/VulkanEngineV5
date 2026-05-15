module;

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <vulkan/vulkan.hpp>
#include <FileLoader/Types.hpp>

export module VulkanEngine.FileLoaders.TextureLoaders;

export namespace VulkanEngine::FileLoaders::Textures {

struct TextureData {
    uint32_t width = 0; //NOLINT(misc-non-private-member-variables-in-classes)
    uint32_t height = 0; //NOLINT(misc-non-private-member-variables-in-classes)
    uint32_t mip_levels = 1; //NOLINT(misc-non-private-member-variables-in-classes)
    uint32_t layer_count = 1; //NOLINT(misc-non-private-member-variables-in-classes)
    uint32_t face_count = 1; //NOLINT(misc-non-private-member-variables-in-classes)
    vk::Format vk_format = vk::Format::eUndefined; //NOLINT(misc-non-private-member-variables-in-classes)
    std::vector<std::byte> pixels{}; //NOLINT(misc-non-private-member-variables-in-classes)

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
                                            const ::FileLoader::ByteBuffer& buffer,
                                            TextureData& out,
                                            std::string* error_message = nullptr);

[[nodiscard]] bool LoadStbTextureFromBuffer(const std::filesystem::path& path,
                                           const ::FileLoader::ByteBuffer& buffer,
                                           TextureData& out,
                                           std::string* error_message = nullptr);

[[nodiscard]] bool LoadTextureFromBuffer(const std::filesystem::path& path,
                                         const ::FileLoader::ByteBuffer& buffer,
                                         TextureData& out,
                                         std::string* error_message = nullptr);

}  // namespace VulkanEngine::FileLoaders::Textures
