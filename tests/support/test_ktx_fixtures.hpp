#pragma once

// In-test KTX2 fixtures built with libktx, shared by the device-free loader
// tests and the GPU sampling tests.
//
// The compressed-format tests need one pixel source that reaches the GPU two
// ways: as RGBA8 (the reference) and through the production Basis->BCn path (the
// case under test). libktx cannot encode native BCn from pixels, so the only way
// to put the same authored pixels into BCn is to compress to Basis and transcode
// both copies: one to RGBA32 for the reference, one to the resolver's BCn target
// via the production TranscodeBasisToTarget. Building both from one in-memory
// image guarantees they share content.

#include <vulkan/vulkan_core.h>

#include <ktx.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace TestSupport {

inline void KtxDestroy(ktxTexture* texture) noexcept {
    ktxTexture_Destroy(texture);
}

// Deterministic, structured test image: enough per-channel variation that a
// compressed format's error is measurable and the render is stable. `alpha`
// chooses whether alpha varies (libktx drops a constant alpha channel, so an
// opaque image is encoded as 3-channel RGB even from an RGBA8 source).
[[nodiscard]] inline std::vector<std::uint8_t> MakeTestImage(std::uint32_t width,
                                                             std::uint32_t height,
                                                             bool varied_alpha = false) {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4U);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * width + x) * 4U;
            pixels[i + 0] = static_cast<std::uint8_t>(x * 255U / (width > 1 ? width - 1 : 1));
            pixels[i + 1] = static_cast<std::uint8_t>(y * 255U / (height > 1 ? height - 1 : 1));
            pixels[i + 2] = static_cast<std::uint8_t>((x + y) & 0xFFU);
            pixels[i + 3] = varied_alpha
                ? static_cast<std::uint8_t>(64U + (x * 191U / (width > 1 ? width - 1 : 1)))
                : 255U;
        }
    }
    return pixels;
}

// Box-downsamples an RGBA8 image to half size (nearest for odd dims).
[[nodiscard]] inline std::vector<std::uint8_t> DownsampleRgba8(const std::vector<std::uint8_t>& src,
                                                               std::uint32_t width,
                                                               std::uint32_t height,
                                                               std::uint32_t& out_width,
                                                               std::uint32_t& out_height) {
    out_width = std::max(1U, width / 2U);
    out_height = std::max(1U, height / 2U);
    std::vector<std::uint8_t> dst(static_cast<std::size_t>(out_width) * out_height * 4U);
    for (std::uint32_t y = 0; y < out_height; ++y) {
        for (std::uint32_t x = 0; x < out_width; ++x) {
            for (std::uint32_t c = 0; c < 4U; ++c) {
                const std::size_t src_i = (static_cast<std::size_t>(y * 2U) * width + x * 2U) * 4U + c;
                dst[(static_cast<std::size_t>(y) * out_width + x) * 4U + c] = src[src_i];
            }
        }
    }
    return dst;
}

// Builds a KTX2 RGBA8 container (single layer/face) with `mip_levels` levels.
[[nodiscard]] inline std::vector<std::byte> MakeKtx2Rgba8(std::uint32_t width,
                                                          std::uint32_t height,
                                                          std::uint32_t mip_levels,
                                                          const std::vector<std::uint8_t>& pixels) {
    ktxTextureCreateInfo info{};
    info.vkFormat = VK_FORMAT_R8G8B8A8_UNORM;
    info.baseWidth = width;
    info.baseHeight = height;
    info.baseDepth = 1;
    info.numDimensions = 2;
    info.numLevels = mip_levels;
    info.numLayers = 1;
    info.numFaces = 1;
    info.isArray = KTX_FALSE;
    info.generateMipmaps = KTX_FALSE;

    ktxTexture2* texture = nullptr;
    if (ktxTexture2_Create(&info, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &texture) != KTX_SUCCESS) {
        throw std::runtime_error("MakeKtx2Rgba8: ktxTexture2_Create failed");
    }

    std::vector<std::uint8_t> level = pixels;
    std::uint32_t level_w = width;
    std::uint32_t level_h = height;
    for (std::uint32_t mip = 0; mip < mip_levels; ++mip) {
        const ktx_size_t size = static_cast<ktx_size_t>(level.size());
        if (ktxTexture_SetImageFromMemory(reinterpret_cast<ktxTexture*>(texture), mip, 0, 0,
                                          level.data(), size) != KTX_SUCCESS) {
            KtxDestroy(reinterpret_cast<ktxTexture*>(texture));
            throw std::runtime_error("MakeKtx2Rgba8: SetImageFromMemory failed");
        }
        if (mip + 1U < mip_levels) {
            level = DownsampleRgba8(level, level_w, level_h, level_w, level_h);
        }
    }

    ktx_uint8_t* bytes = nullptr;
    ktx_size_t size = 0;
    if (ktxTexture_WriteToMemory(reinterpret_cast<ktxTexture*>(texture), &bytes, &size) != KTX_SUCCESS) {
        KtxDestroy(reinterpret_cast<ktxTexture*>(texture));
        throw std::runtime_error("MakeKtx2Rgba8: WriteToMemory failed");
    }
    std::vector<std::byte> out(reinterpret_cast<std::byte*>(bytes),
                               reinterpret_cast<std::byte*>(bytes) + size);
    KtxDestroy(reinterpret_cast<ktxTexture*>(texture));
    std::free(bytes);
    return out;
}

// Builds a KTX2 Basis UASTC container directly from pixels, in a single write,
// producing a container the loader keeps as needs_transcode. Returns empty on
// failure.
//
// `channels` selects the encoded channel model, which is what the resolver's
// format rules key off: 4 (RGBA), 3 (RGB, opaque), 2 (two independent channels,
// RG -> BC5 for a Standard normal), 1 (single channel -> BC4 for data). libktx
// derives the DFD channel model from the input swizzle plus, for 1/2-channel
// content, its normal-map tuning; matching that here is what lets the BC5/BC4
// rules be reached at all.
//
// Built straight from pixels rather than by re-reading a written RGBA8
// container: libktx's writer leaks when a container that already carries a
// writer key is written again (writer2.c appendLibId returns before freeing its
// temporary on the "already present" path), which the two-step build tripped.
[[nodiscard]] inline std::vector<std::byte> MakeBasisUastc(
    std::uint32_t width, std::uint32_t height, std::uint32_t mip_levels,
    const std::vector<std::uint8_t>& pixels, std::uint32_t channels = 4) {
    ktxTextureCreateInfo info{};
    info.vkFormat = VK_FORMAT_R8G8B8A8_UNORM;
    info.baseWidth = width;
    info.baseHeight = height;
    info.baseDepth = 1;
    info.numDimensions = 2;
    info.numLevels = mip_levels;
    info.numLayers = 1;
    info.numFaces = 1;
    info.isArray = KTX_FALSE;
    info.generateMipmaps = KTX_FALSE;

    ktxTexture2* texture = nullptr;
    if (ktxTexture2_Create(&info, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &texture) != KTX_SUCCESS) {
        return {};
    }

    std::vector<std::uint8_t> level = pixels;
    std::uint32_t level_w = width;
    std::uint32_t level_h = height;
    for (std::uint32_t mip = 0; mip < mip_levels; ++mip) {
        const ktx_size_t size = static_cast<ktx_size_t>(level.size());
        if (ktxTexture_SetImageFromMemory(reinterpret_cast<ktxTexture*>(texture), mip, 0, 0,
                                          level.data(), size) != KTX_SUCCESS) {
            KtxDestroy(reinterpret_cast<ktxTexture*>(texture));
            return {};
        }
        if (mip + 1U < mip_levels) {
            level = DownsampleRgba8(level, level_w, level_h, level_w, level_h);
        }
    }

    ktxBasisParams params{};
    params.structSize = sizeof(params);
    params.uastc = KTX_TRUE;
    params.threadCount = 1;
    // Level 1 (faster) keeps the test fast; quality is measured in the render.
    params.uastcFlags = KTX_PACK_UASTC_LEVEL_FASTER;
    switch (channels) {
        case 4:
            std::memcpy(params.inputSwizzle, "rgba", 4);
            break;
        case 3:
            std::memcpy(params.inputSwizzle, "rgb1", 4);
            break;
        case 2:
            std::memcpy(params.inputSwizzle, "rrrg", 4);
            params.normalMap = KTX_TRUE;
            break;
        case 1:
            std::memcpy(params.inputSwizzle, "rrr1", 4);
            params.normalMap = KTX_TRUE;
            break;
        default:
            KtxDestroy(reinterpret_cast<ktxTexture*>(texture));
            return {};
    }
    if (ktxTexture2_CompressBasisEx(texture, &params) != KTX_SUCCESS) {
        KtxDestroy(reinterpret_cast<ktxTexture*>(texture));
        return {};
    }
    ktx_uint8_t* bytes = nullptr;
    ktx_size_t size = 0;
    if (ktxTexture_WriteToMemory(reinterpret_cast<ktxTexture*>(texture), &bytes, &size) != KTX_SUCCESS) {
        KtxDestroy(reinterpret_cast<ktxTexture*>(texture));
        return {};
    }
    std::vector<std::byte> out(reinterpret_cast<std::byte*>(bytes),
                               reinterpret_cast<std::byte*>(bytes) + size);
    KtxDestroy(reinterpret_cast<ktxTexture*>(texture));
    std::free(bytes);
    return out;
}

}  // namespace TestSupport
