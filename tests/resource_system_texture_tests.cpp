#include <gtest/gtest.h>

#include <ktx.h>

#include <vulkan/vulkan.hpp>

#include <array>
#include <cstddef>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>

import VulkanEngine.ResourceSystem;
import VulkanEngine.ResourceSystem.TextureResource;

namespace {

class TempKtx2File final {
public:
    TempKtx2File() {
        const auto stamp = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        std::random_device rd;
        path_ = std::filesystem::temp_directory_path() /
                ("ve5_texture_resource_" + std::to_string(stamp) + "_" + std::to_string(rd()) + ".ktx2");
    }

    ~TempKtx2File() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }

    [[nodiscard]] const std::filesystem::path& Path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

[[nodiscard]] std::array<std::uint8_t, 16> Make2x2RgbaPixels() {
    return {
        255, 0, 0, 255,
        0, 255, 0, 255,
        0, 0, 255, 255,
        255, 255, 0, 255,
    };
}

void WriteTinyKtx2TextureToTempFile(const TempKtx2File& temp) {

    ktxTextureCreateInfo info{};
    info.vkFormat = static_cast<ktx_uint32_t>(VK_FORMAT_R8G8B8A8_UNORM);
    info.baseWidth = 2;
    info.baseHeight = 2;
    info.baseDepth = 1;
    info.numDimensions = 2;
    info.numLevels = 1;
    info.numLayers = 1;
    info.numFaces = 1;
    info.isArray = KTX_FALSE;
    info.generateMipmaps = KTX_FALSE;

    ktxTexture2* texture = nullptr;
    const KTX_error_code create_result = ktxTexture2_Create(&info, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &texture);
    if (create_result != KTX_SUCCESS || texture == nullptr) {
        throw std::runtime_error(std::string("ktxTexture2_Create failed: ") + ktxErrorString(create_result));
    }

    const auto pixels = Make2x2RgbaPixels();
    const KTX_error_code image_result = ktxTexture_SetImageFromMemory(
        reinterpret_cast<ktxTexture*>(texture),
        0,
        0,
        0,
        pixels.data(),
        static_cast<ktx_size_t>(pixels.size()));
    if (image_result != KTX_SUCCESS) {
        ktxTexture_Destroy(reinterpret_cast<ktxTexture*>(texture));
        throw std::runtime_error(std::string("ktxTexture_SetImageFromMemory failed: ") + ktxErrorString(image_result));
    }

    ktx_uint8_t* out_bytes = nullptr;
    ktx_size_t out_size = 0;
    const KTX_error_code write_result = ktxTexture_WriteToMemory(reinterpret_cast<ktxTexture*>(texture), &out_bytes, &out_size);
    if (write_result != KTX_SUCCESS || out_bytes == nullptr || out_size == 0) {
        ktxTexture_Destroy(reinterpret_cast<ktxTexture*>(texture));
        throw std::runtime_error(std::string("ktxTexture_WriteToMemory failed: ") + ktxErrorString(write_result));
    }

    {
        std::ofstream out(temp.Path(), std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(out_bytes), static_cast<std::streamsize>(out_size));
    }

    std::free(out_bytes);
    ktxTexture_Destroy(reinterpret_cast<ktxTexture*>(texture));
}

class TextureResourceTest : public ::testing::Test {};

}  // namespace

TEST_F(TextureResourceTest, LoadsTinyRgbaKtx2) {
    const TempKtx2File temp;
    WriteTinyKtx2TextureToTempFile(temp);

    VulkanEngine::ResourceManager manager;
    auto handle = manager.LoadFromFile<VulkanEngine::TextureResource>(
        temp.Path(),
        VulkanEngine::ResourceManager::LoadSpeed::Instant);

    ASSERT_TRUE(handle.IsValid());
    auto* texture = handle.Get();
    ASSERT_NE(texture, nullptr);

    EXPECT_EQ(texture->GetWidth(), 2u);
    EXPECT_EQ(texture->GetHeight(), 2u);
    EXPECT_EQ(texture->GetMipLevels(), 1u);
    EXPECT_EQ(texture->GetLayerCount(), 1u);
    EXPECT_EQ(texture->GetFaceCount(), 1u);
    EXPECT_EQ(texture->GetVkFormat(), vk::Format::eR8G8B8A8Unorm);
    EXPECT_TRUE(texture->HasPixels());
    EXPECT_EQ(texture->GetPixels().size(), 16u);

    const auto expected = Make2x2RgbaPixels();
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(texture->GetPixels()[i], static_cast<std::byte>(expected[i])) << "mismatch at byte " << i;
    }

    manager.Release(handle.GetId());
}

TEST_F(TextureResourceTest, RejectsInvalidFile) {
    const TempKtx2File temp;
    {
        std::ofstream out(temp.Path(), std::ios::binary | std::ios::trunc);
        const std::array<std::uint8_t, 8> junk{1, 2, 3, 4, 5, 6, 7, 8};
        out.write(reinterpret_cast<const char*>(junk.data()), static_cast<std::streamsize>(junk.size()));
    }

    VulkanEngine::ResourceManager manager;
    auto handle = manager.LoadFromFile<VulkanEngine::TextureResource>(
        temp.Path(),
        VulkanEngine::ResourceManager::LoadSpeed::Instant);
    EXPECT_FALSE(handle.IsValid());
}






