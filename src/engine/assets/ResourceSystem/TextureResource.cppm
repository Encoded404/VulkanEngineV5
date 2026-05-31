module;

#include <cstddef>
#include <cstdint>
#include <FileLoader/Types.hpp>
#include <string>
#include <vector>
#include <array>

#include <vulkan/vulkan.hpp>

export module VulkanEngine.ResourceSystem.TextureResource;

import VulkanEngine.ResourceSystem;
import VulkanEngine.FileLoaders.TextureLoaders;

export namespace VulkanEngine {

struct CheckerboardConfig {
    uint32_t size = 64;
    uint32_t squares = 8;
    std::array<uint8_t, 4> color1 = {255, 255, 255, 255}; // White
    std::array<uint8_t, 4> color2 = {255, 0, 255, 255};   // Purple
};

class TextureResource final : public Resource {
public:
    explicit TextureResource(ResourceId id);

    TextureResource(ResourceId id, uint32_t width, uint32_t height, vk::Format format,
                    std::vector<std::byte> pixels);

    [[nodiscard]] uint32_t GetWidth() const noexcept;
    [[nodiscard]] uint32_t GetHeight() const noexcept;
    [[nodiscard]] uint32_t GetMipLevels() const noexcept;
    [[nodiscard]] uint32_t GetLayerCount() const noexcept;
    [[nodiscard]] uint32_t GetFaceCount() const noexcept;
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
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t mip_levels_ = 0;
    uint32_t layer_count_ = 0;
    uint32_t face_count_ = 0;
    vk::Format vk_format_ = vk::Format::eUndefined;
    bool transcoded_ = false;
    VulkanEngine::FileLoaders::Textures::AlphaAnalysis alpha_analysis_{};
    std::vector<std::byte> pixels_{};
    // NOLINTEND(misc-non-private-member-variables-in-classes)
};

}  // namespace VulkanEngine
