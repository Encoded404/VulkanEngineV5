module;

#include <SDL3/SDL_video.h>

export module VulkanBackend.Vulkan.CommonTypes;

import std;

export namespace VulkanBackend::Vulkan {

enum class PresentMode : std::uint8_t {
    Mailbox,
    Fifo,
    Immediate,
    FifoRelaxed
};

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
    bool enable_validation = true;
    std::uint32_t frames_in_flight = 3;
    std::uint32_t preferred_swapchain_image_count = 3;
    PresentMode present_mode = PresentMode::Mailbox;
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
    std::uint32_t swapchain_image_count = 0;
    std::uint32_t swapchain_width = 0;
    std::uint32_t swapchain_height = 0;
    BootstrapStatus status = BootstrapStatus::NotInitialized;
    std::string error_message;
};

} // namespace VulkanBackend::Vulkan
