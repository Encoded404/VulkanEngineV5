#include <gtest/gtest.h>

#include <array>
#include <functional>
#include <span>
#include <vector>

import vulkan;

import VulkanBackend.Vulkan.CommonTypes;

namespace {

using VulkanBackend::Vulkan::ColorMode;
using VulkanBackend::Vulkan::EncodingForSurfaceFormat;
using VulkanBackend::Vulkan::OutputEncoding;
using VulkanBackend::Vulkan::SelectOutputTarget;

[[nodiscard]] vk::SurfaceFormatKHR Pair(vk::Format format, vk::ColorSpaceKHR color_space) {
    return vk::SurfaceFormatKHR{format, color_space};
}

[[nodiscard]] std::function<bool(vk::Format)> AllSupported() {
    return [](vk::Format) { return true; };
}

// The first pairs a KWin Wayland session advertises while an output is in HDR mode, in the compositor's order: 16-bit scanout formats first, 8-bit sRGB last.
// The 16-bit UNORM pair is SDR and needs an explicit sRGB OETF.
[[nodiscard]] std::vector<vk::SurfaceFormatKHR> KWinHdrSurfaceList() {
    return {
        Pair(vk::Format::eR16G16B16A16Unorm, vk::ColorSpaceKHR::eSrgbNonlinear),
        Pair(vk::Format::eA2B10G10R10UnormPack32, vk::ColorSpaceKHR::eSrgbNonlinear),
        Pair(vk::Format::eR16G16B16A16Sfloat, vk::ColorSpaceKHR::eSrgbNonlinear),
        Pair(vk::Format::eB8G8R8A8Unorm, vk::ColorSpaceKHR::eSrgbNonlinear),
        Pair(vk::Format::eB8G8R8A8Srgb, vk::ColorSpaceKHR::eSrgbNonlinear),
        Pair(vk::Format::eR16G16B16A16Sfloat, vk::ColorSpaceKHR::eExtendedSrgbLinearEXT),
        Pair(vk::Format::eA2B10G10R10UnormPack32, vk::ColorSpaceKHR::eHdr10St2084EXT),
    };
}

TEST(OutputColorTest, ClassifiesTransferOwnership) {
    EXPECT_EQ(EncodingForSurfaceFormat(
                  Pair(vk::Format::eB8G8R8A8Srgb, vk::ColorSpaceKHR::eSrgbNonlinear)),
              OutputEncoding::HardwareSrgb);
    EXPECT_EQ(EncodingForSurfaceFormat(
                  Pair(vk::Format::eR16G16B16A16Unorm, vk::ColorSpaceKHR::eSrgbNonlinear)),
              OutputEncoding::ShaderSrgb);
    EXPECT_EQ(EncodingForSurfaceFormat(
                  Pair(vk::Format::eB8G8R8A8Unorm, vk::ColorSpaceKHR::eSrgbNonlinear)),
              OutputEncoding::ShaderSrgb);
    EXPECT_EQ(EncodingForSurfaceFormat(
                  Pair(vk::Format::eA8B8G8R8SrgbPack32, vk::ColorSpaceKHR::eSrgbNonlinear)),
              OutputEncoding::HardwareSrgb);
    EXPECT_EQ(EncodingForSurfaceFormat(
                  Pair(vk::Format::eR16G16B16A16Sfloat,
                       vk::ColorSpaceKHR::eExtendedSrgbLinearEXT)),
              OutputEncoding::Scrgb);
    EXPECT_EQ(EncodingForSurfaceFormat(
                  Pair(vk::Format::eA2B10G10R10UnormPack32,
                       vk::ColorSpaceKHR::eHdr10St2084EXT)),
              OutputEncoding::Pq);
}

TEST(OutputColorTest, SdrNeverTakesTheCompositorFirstPair) {
    const auto formats = KWinHdrSurfaceList();
    const auto target = SelectOutputTarget(formats, ColorMode::Sdr, AllSupported());

    ASSERT_EQ(target.surface_format.format, vk::Format::eB8G8R8A8Srgb);
    ASSERT_EQ(target.surface_format.colorSpace, vk::ColorSpaceKHR::eSrgbNonlinear);
    EXPECT_EQ(target.encoding, OutputEncoding::HardwareSrgb);
    EXPECT_FALSE(target.fallback);
}

TEST(OutputColorTest, Hdr10SelectsTenBitPgPair) {
    const auto formats = KWinHdrSurfaceList();
    const auto target = SelectOutputTarget(formats, ColorMode::Hdr10, AllSupported());

    ASSERT_EQ(target.surface_format.format, vk::Format::eA2B10G10R10UnormPack32);
    ASSERT_EQ(target.surface_format.colorSpace, vk::ColorSpaceKHR::eHdr10St2084EXT);
    EXPECT_EQ(target.encoding, OutputEncoding::Pq);
    EXPECT_FALSE(target.fallback);
}

TEST(OutputColorTest, ScrgbSelectsLinearFloatPair) {
    const auto formats = KWinHdrSurfaceList();
    const auto target = SelectOutputTarget(formats, ColorMode::HdrScrgb, AllSupported());

    ASSERT_EQ(target.surface_format.format, vk::Format::eR16G16B16A16Sfloat);
    ASSERT_EQ(target.surface_format.colorSpace, vk::ColorSpaceKHR::eExtendedSrgbLinearEXT);
    EXPECT_EQ(target.encoding, OutputEncoding::Scrgb);
    EXPECT_FALSE(target.fallback);
}

TEST(OutputColorTest, HdrRequestFallsBackToSdrWhenUnadvertised) {
    const std::array formats{
        Pair(vk::Format::eB8G8R8A8Srgb, vk::ColorSpaceKHR::eSrgbNonlinear),
        Pair(vk::Format::eB8G8R8A8Unorm, vk::ColorSpaceKHR::eSrgbNonlinear),
    };
    const auto target = SelectOutputTarget(formats, ColorMode::Hdr10, AllSupported());

    ASSERT_EQ(target.surface_format.format, vk::Format::eB8G8R8A8Srgb);
    EXPECT_EQ(target.encoding, OutputEncoding::HardwareSrgb);
    EXPECT_TRUE(target.fallback);
}

TEST(OutputColorTest, PackedSrgbFormatKeepsHardwareEncode) {
    const std::array formats{
        Pair(vk::Format::eA8B8G8R8SrgbPack32, vk::ColorSpaceKHR::eSrgbNonlinear),
    };
    const auto target = SelectOutputTarget(formats, ColorMode::Sdr, AllSupported());

    ASSERT_EQ(target.surface_format.format, vk::Format::eA8B8G8R8SrgbPack32);
    EXPECT_EQ(target.encoding, OutputEncoding::HardwareSrgb);
}

TEST(OutputColorTest, NoSrgbFormatFallsBackToShaderEncode) {
    const std::array formats{
        Pair(vk::Format::eR16G16B16A16Unorm, vk::ColorSpaceKHR::eSrgbNonlinear),
        Pair(vk::Format::eB8G8R8A8Unorm, vk::ColorSpaceKHR::eSrgbNonlinear),
    };
    const auto target = SelectOutputTarget(formats, ColorMode::Sdr, AllSupported());

    // The engine can use the high-precision surface because the output shader applies the sRGB OETF for it.
    ASSERT_EQ(target.surface_format.format, vk::Format::eB8G8R8A8Unorm);
    EXPECT_EQ(target.encoding, OutputEncoding::ShaderSrgb);
}

TEST(OutputColorTest, SkipsUnsupportedFormats) {
    const auto formats = KWinHdrSurfaceList();
    const auto only_16bit = [](vk::Format format) {
        return format == vk::Format::eR16G16B16A16Unorm ||
               format == vk::Format::eR16G16B16A16Sfloat;
    };
    const auto target = SelectOutputTarget(formats, ColorMode::Sdr, only_16bit);

    ASSERT_EQ(target.surface_format.format, vk::Format::eR16G16B16A16Unorm);
    EXPECT_EQ(target.encoding, OutputEncoding::ShaderSrgb);
}

TEST(OutputColorTest, EmptySurfaceListStaysUndefined) {
    const std::array<vk::SurfaceFormatKHR, 0> formats{};
    const auto target = SelectOutputTarget(formats, ColorMode::Sdr, AllSupported());

    EXPECT_EQ(target.surface_format.format, vk::Format::eUndefined);
}

} // namespace
