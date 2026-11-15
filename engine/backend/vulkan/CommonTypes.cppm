module;

#include <SDL3/SDL_video.h>

export module VulkanBackend.Vulkan.CommonTypes;

import std;

import vulkan;

export namespace VulkanBackend::Vulkan {

enum class PresentMode : std::uint8_t {
    Mailbox,
    Fifo,
    Immediate,
    FifoRelaxed
};

// The engine renders into a linear, scene-referred FP16 target. The swapchain is only the output encoding; ColorMode asks the backend for a particular display-referred output, and the actual choice is made from the surface's (format, colorSpace) pairs (see SelectOutputTarget).
enum class ColorMode : std::uint8_t {
    // SDR sRGB. Safe on every display; the compositor maps it to its SDR white.
    Sdr,
    // scRGB: linear FP16, sRGB primaries, values above 1.0 for HDR.
    HdrScrgb,
    // HDR10: Rec.2020 primaries, ST.2084 PQ, 10-bit UNORM.
    Hdr10,
    // Automatic mode is deliberately conservative: SDR until the application opts into HDR.
    // It exists so the public config has a non-committal default.
    Auto
};

// The transfer function the final output pass must apply. This is a property of the chosen (format, colorSpace) pair, never of the requested ColorMode: a request for HDR10 that falls back to SDR is still encoded as SDR.
enum class OutputEncoding : std::uint8_t {
    // Linear values written to an _SRGB image: the hardware applies the sRGB OETF on store.
    HardwareSrgb,
    // Linear values written to a UNORM/FP16 image with SRGB_NONLINEAR: the output shader applies the sRGB OETF.
    ShaderSrgb,
    // scRGB: linear values in an FP16 image with EXTENDED_SRGB_LINEAR_EXT.
    Scrgb,
    // HDR10: the output shader applies the Rec.709->Rec.2020 matrix and the ST.2084 PQ OETF.
    Pq
};

// The single scene/composition color format. The scene, world text, screen text and ImGui all write here; only the output pass touches the swapchain.
// Keeping it FP16 linear is what lets SDR and HDR share every scene pipeline.
inline constexpr vk::Format kSceneColorFormat = vk::Format::eR16G16B16A16Sfloat;

// HDR presentation settings. Paper white is the diffuse-white level the scene's 1.0 maps to; peak is the display's highlight ceiling.
// Both are in cd/m^2; ignored in SDR mode.
struct OutputSettings {
    ColorMode mode = ColorMode::Sdr;
    // Exposure multiplier applied before the output transfer function.
    float exposure = 1.0f;
    float paper_white_nits = 100.0f;
    // <= paper_white_nits means "use the engine default" (1000 nits).
    float peak_nits = 1000.0f;
};

struct OutputTarget {
    vk::SurfaceFormatKHR surface_format{};
    OutputEncoding encoding = OutputEncoding::ShaderSrgb;
    ColorMode mode = ColorMode::Sdr;
    // True when the requested mode was unavailable and a lower mode was used.
    bool fallback = false;
};

// True for the sRGB-encoded image formats whose stores the hardware converts from linear.
// This is the only property that decides whether an output shader must apply the sRGB OETF itself.
[[nodiscard]] inline bool IsSrgbFormat(vk::Format format) {
    switch (format) {
        case vk::Format::eR8G8B8Srgb:
        case vk::Format::eB8G8R8Srgb:
        case vk::Format::eR8G8B8A8Srgb:
        case vk::Format::eB8G8R8A8Srgb:
        case vk::Format::eA8B8G8R8SrgbPack32:
            return true;
        default:
            return false;
    }
}

// The encode the presentation engine expects for this pair. The color space decides the transfer function; the image format only decides who applies it.
[[nodiscard]] inline OutputEncoding EncodingForSurfaceFormat(
    const vk::SurfaceFormatKHR& surface_format) {
    if (surface_format.colorSpace == vk::ColorSpaceKHR::eHdr10St2084EXT) {
        return OutputEncoding::Pq;
    }
    if (surface_format.colorSpace == vk::ColorSpaceKHR::eExtendedSrgbLinearEXT) {
        return OutputEncoding::Scrgb;
    }
    return IsSrgbFormat(surface_format.format) ? OutputEncoding::HardwareSrgb
                                               : OutputEncoding::ShaderSrgb;
}

namespace OutputTargetDetail {

inline const vk::SurfaceFormatKHR* FindPair(
    std::span<const vk::SurfaceFormatKHR> formats,
    vk::ColorSpaceKHR color_space,
    std::span<const vk::Format> preferred,
    const std::function<bool(vk::Format)>& supported) {
    for (const vk::Format format : preferred) {
        if (!supported(format)) {
            continue;
        }
        for (const auto& pair : formats) {
            if (pair.format == format && pair.colorSpace == color_space) {
                return &pair;
            }
        }
    }
    return nullptr;
}

} // namespace OutputTargetDetail

// Chooses the swapchain pair for `requested` from the formats the surface advertises.
// The order of `formats` is a compositor preference, not a quality ranking (on a KWin HDR session the first pair is 16-bit UNORM), so every candidate is scored against the requested mode instead.
//
// `supported` filters out formats the device cannot render to (the caller uses the physical-device format properties).
// The function is pure so the fallback chain is testable without a device.
[[nodiscard]] inline OutputTarget SelectOutputTarget(
    std::span<const vk::SurfaceFormatKHR> formats,
    ColorMode requested,
    const std::function<bool(vk::Format)>& supported) {
    constexpr std::array<vk::Format, 5> kSrgbFormats{
        vk::Format::eB8G8R8A8Srgb,
        vk::Format::eR8G8B8A8Srgb,
        vk::Format::eA8B8G8R8SrgbPack32,
        vk::Format::eB8G8R8Srgb,
        vk::Format::eR8G8B8Srgb,
    };
    constexpr std::array<vk::Format, 6> kSdrFallbackFormats{
        vk::Format::eA2B10G10R10UnormPack32,
        vk::Format::eA2R10G10B10UnormPack32,
        vk::Format::eB8G8R8A8Unorm,
        vk::Format::eR8G8B8A8Unorm,
        vk::Format::eR16G16B16A16Unorm,
        vk::Format::eR16G16B16A16Sfloat,
    };
    constexpr std::array<vk::Format, 2> kHdr10Formats{
        vk::Format::eA2B10G10R10UnormPack32,
        vk::Format::eA2R10G10B10UnormPack32,
    };
    constexpr std::array<vk::Format, 2> kScrgbFormats{
        vk::Format::eR16G16B16A16Sfloat,
        vk::Format::eR32G32B32A32Sfloat,
    };

    const auto pick_sdr = [&]() -> const vk::SurfaceFormatKHR* {
        if (const auto* pair = OutputTargetDetail::FindPair(
                formats, vk::ColorSpaceKHR::eSrgbNonlinear, kSrgbFormats, supported)) {
            return pair;
        }
        if (const auto* pair = OutputTargetDetail::FindPair(
                formats, vk::ColorSpaceKHR::eSrgbNonlinear, kSdrFallbackFormats, supported)) {
            return pair;
        }
        for (const auto& pair : formats) {
            if (pair.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear && supported(pair.format)) {
                return &pair;
            }
        }
        return nullptr;
    };
    const auto pick_hdr10 = [&]() -> const vk::SurfaceFormatKHR* {
        return OutputTargetDetail::FindPair(formats, vk::ColorSpaceKHR::eHdr10St2084EXT,
                                            kHdr10Formats, supported);
    };
    const auto pick_scrgb = [&]() -> const vk::SurfaceFormatKHR* {
        return OutputTargetDetail::FindPair(formats, vk::ColorSpaceKHR::eExtendedSrgbLinearEXT,
                                            kScrgbFormats, supported);
    };

    const auto make_target = [&](const vk::SurfaceFormatKHR* pair,
                                 bool fallback) -> OutputTarget {
        if (pair == nullptr) {
            return OutputTarget{};
        }
        OutputTarget target{};
        target.surface_format = *pair;
        target.encoding = EncodingForSurfaceFormat(*pair);
        target.mode = requested;
        target.fallback = fallback;
        return target;
    };

    if (requested == ColorMode::Hdr10) {
        if (const auto* pair = pick_hdr10()) {
            return make_target(pair, false);
        }
        if (const auto* pair = pick_scrgb()) {
            return make_target(pair, true);
        }
    } else if (requested == ColorMode::HdrScrgb) {
        if (const auto* pair = pick_scrgb()) {
            return make_target(pair, false);
        }
    }

    if (const auto* pair = pick_sdr()) {
        return make_target(pair, requested == ColorMode::Hdr10 ||
                                      requested == ColorMode::HdrScrgb);
    }

    // No sRGB color space at all. Use the first renderable pair rather than failing startup.
    // EncodingForSurfaceFormat still picks the correct transfer for the ones the engine knows.
    for (const auto& pair : formats) {
        if (supported(pair.format)) {
            return make_target(&pair, true);
        }
    }
    return OutputTarget{};
}

enum class BootstrapStatus : std::uint8_t {
    Ok,
    NotInitialized,
    RequirementsUnmet,
    InstanceCreationFailed,
    DeviceSelectionFailed,
    DeviceCreationFailed,
    SwapchainCreationFailed,
    SwapchainOutOfDate,
    DeviceLost,
    FatalError
};

struct ExtensionRequest {
    std::string name;
    bool required = false;   // missing ⇒ startup failure; else warn + skip
};

struct VulkanBootstrapConfig {
    std::uint32_t api_major = 1;
    std::uint32_t api_minor = 3;
    std::uint32_t api_patch = 0;
#ifdef NDEBUG
    bool enable_validation = false;
#else
    bool enable_validation = true;
#endif
    std::uint32_t frames_in_flight = 3;
    std::uint32_t preferred_swapchain_image_count = 3;
    PresentMode present_mode = PresentMode::Mailbox;
    // SDR by default. Set mode to Hdr10 or HdrScrgb to opt into HDR; the backend falls back to SDR when the surface does not advertise it.
    OutputSettings output{};
    SDL_Window* native_window_handle = nullptr;
    std::vector<ExtensionRequest> instance_extensions;   // additive, validated
    std::vector<ExtensionRequest> device_extensions;     // additive, validated
    std::vector<std::string> force_disabled_extensions;  // test degradation paths
};

struct VulkanBootstrapState {
    bool instance_ready = false;
    bool device_ready = false;
    bool swapchain_ready = false;
    std::uint32_t frame_index = 0;
    std::uint32_t frames_in_flight = 3;
    std::uint32_t swapchain_image_count = 0;
    std::uint32_t swapchain_width = 0;
    std::uint32_t swapchain_height = 0;
    BootstrapStatus status = BootstrapStatus::NotInitialized;
    std::string error_message;
};

} // namespace VulkanBackend::Vulkan
