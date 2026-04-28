module;

#include <cstddef>
#include <cstdint>
#include <FileLoader/Types.hpp>
#include <string>
#include <vector>
#include <array>
#include <memory>

#include <vulkan/vulkan.hpp>

export module VulkanEngine.ResourceSystem.TextureResource;

import VulkanEngine.ResourceSystem;

export namespace VulkanEngine {

struct CheckerboardConfig {
    uint32_t size = 64;
    uint32_t squares = 8;
    std::array<uint8_t, 4> color1 = {255, 255, 255, 255}; // White
    std::array<uint8_t, 4> color2 = {255, 0, 255, 255};   // Purple
};

class TextureResource final : public Resource {
public:
    explicit TextureResource(std::string id);

    [[nodiscard]] uint32_t GetWidth() const noexcept;
    [[nodiscard]] uint32_t GetHeight() const noexcept;
    [[nodiscard]] uint32_t GetMipLevels() const noexcept;
    [[nodiscard]] uint32_t GetLayerCount() const noexcept;
    [[nodiscard]] uint32_t GetFaceCount() const noexcept;
    [[nodiscard]] vk::Format GetVkFormat() const noexcept;
    [[nodiscard]] const std::vector<std::byte>& GetPixels() const noexcept;
    [[nodiscard]] bool HasPixels() const noexcept;

    // Static factory method to create a checkerboard texture
    [[nodiscard]] static std::shared_ptr<TextureResource> CreateCheckerboardTexture(std::string id, const CheckerboardConfig& config);

protected:
    bool DoLoad() override;
    bool DoUnload() override;
    bool DoLoadFromBuffer(const FileLoader::ByteBuffer& buf) override;

private:
    void Reset() noexcept;

    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t mip_levels_ = 0;
    uint32_t layer_count_ = 0;
    uint32_t face_count_ = 0;
    vk::Format vk_format_ = vk::Format::eUndefined;
    bool transcoded_ = false;
    std::vector<std::byte> pixels_{};
};

}  // namespace VulkanEngine
