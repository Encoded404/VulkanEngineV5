module;

export module VulkanEngine.FileLoaders.TextureLoaders;

import std;
import std.compat;

import FileLoader.Types;

import vulkan_hpp;

export namespace VulkanEngine::FileLoaders::Textures {

struct AlphaAnalysis {
    bool hasAlphaChannel = false; //NOLINT(misc-non-private-member-variables-in-classes)
    bool hasFractionalAlpha = false; //NOLINT(misc-non-private-member-variables-in-classes)
    bool hasZeroAlpha = false; //NOLINT(misc-non-private-member-variables-in-classes)
    float opaqueCoverage = 1.0f; //NOLINT(misc-non-private-member-variables-in-classes)
};

[[nodiscard]] AlphaAnalysis AnalyzeAlpha(const std::vector<std::byte>& pixels);

struct TextureData {
    std::uint32_t width = 0; //NOLINT(misc-non-private-member-variables-in-classes)
    std::uint32_t height = 0; //NOLINT(misc-non-private-member-variables-in-classes)
    std::uint32_t mip_levels = 1; //NOLINT(misc-non-private-member-variables-in-classes)
    std::uint32_t layer_count = 1; //NOLINT(misc-non-private-member-variables-in-classes)
    std::uint32_t face_count = 1; //NOLINT(misc-non-private-member-variables-in-classes)
    vk::Format vk_format = vk::Format::eUndefined; //NOLINT(misc-non-private-member-variables-in-classes)
    AlphaAnalysis alpha_analysis{}; //NOLINT(misc-non-private-member-variables-in-classes)
    std::vector<std::byte> pixels{}; //NOLINT(misc-non-private-member-variables-in-classes)

    void Reset() noexcept {
        width = 0;
        height = 0;
        mip_levels = 1;
        layer_count = 1;
        face_count = 1;
        vk_format = vk::Format::eUndefined;
        alpha_analysis = {};
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
