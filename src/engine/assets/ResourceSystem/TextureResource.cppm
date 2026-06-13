module;

// NOLINTNEXTLINE(misc-include-cleaner)
#include <memory>  // workaround for LLVM #138558: friend/using-decl conflict in bits/shared_ptr.h

export module VulkanEngine.ResourceSystem.TextureResource;

import std;
import std.compat;

import FileLoader.Types;

import vulkan_hpp;

import VulkanEngine.ResourceSystem;
import VulkanEngine.FileLoaders.TextureLoaders;

export namespace VulkanEngine {

struct CheckerboardConfig {
    std::uint32_t size = 64;
    std::uint32_t squares = 8;
    std::array<std::uint8_t, 4> color1 = {255, 255, 255, 255}; // White
    std::array<std::uint8_t, 4> color2 = {255, 0, 255, 255};   // Purple
};

class TextureResource final : public Resource {
public:
    explicit TextureResource(ResourceId id);

    TextureResource(ResourceId id, std::uint32_t width, std::uint32_t height, vk::Format format,
                    std::vector<std::byte> pixels);

    [[nodiscard]] std::uint32_t GetWidth() const noexcept;
    [[nodiscard]] std::uint32_t GetHeight() const noexcept;
    [[nodiscard]] std::uint32_t GetMipLevels() const noexcept;
    [[nodiscard]] std::uint32_t GetLayerCount() const noexcept;
    [[nodiscard]] std::uint32_t GetFaceCount() const noexcept;
    [[nodiscard]] vk::Format GetVkFormat() const noexcept;
    [[nodiscard]] const std::vector<std::byte>& GetPixels() const noexcept;
    [[nodiscard]] bool HasPixels() const noexcept;
    [[nodiscard]] const VulkanEngine::FileLoaders::Textures::AlphaAnalysis& GetAlphaAnalysis() const noexcept;

protected:
    bool DoLoad() override;
    bool DoUnload() override;
    bool DoLoadFromBuffer(const FileLoader::ByteBuffer& buf) override;
    void Reset() noexcept;

private:
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::uint32_t mip_levels_ = 0;
    std::uint32_t layer_count_ = 0;
    std::uint32_t face_count_ = 0;
    vk::Format vk_format_ = vk::Format::eUndefined;
    bool transcoded_ = false;
    VulkanEngine::FileLoaders::Textures::AlphaAnalysis alpha_analysis_{};
    std::vector<std::byte> pixels_{};
    // NOLINTEND(misc-non-private-member-variables-in-classes)
};

}  // namespace VulkanEngine
