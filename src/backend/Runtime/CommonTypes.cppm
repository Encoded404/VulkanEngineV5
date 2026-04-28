module;

#include <cstdint>
#include <SDL3/SDL_video.h>

export module VulkanEngine.Runtime.CommonTypes;

export namespace VulkanEngine::Runtime {

enum class BootstrapStatus : uint8_t {
    Ok,
    NotInitialized,
    InstanceCreationFailed,
    DeviceSelectionFailed,
    DeviceCreationFailed,
    SwapchainCreationFailed,
    SwapchainOutOfDate,
    DeviceLost,
    FatalError
};

struct VulkanBootstrapConfig {
    uint32_t api_major = 1;
    uint32_t api_minor = 3;
    uint32_t api_patch = 0;
    bool enable_validation = true;
    uint32_t frames_in_flight = 2;
    uint32_t preferred_swapchain_image_count = 3;
    SDL_Window* native_window_handle = nullptr;
};

struct VulkanBootstrapSnapshot {
    bool instance_ready = false;
    bool device_ready = false;
    bool swapchain_ready = false;
    uint32_t frame_index = 0;
    uint32_t swapchain_image_count = 0;
    uint32_t swapchain_width = 0;
    uint32_t swapchain_height = 0;
    BootstrapStatus status = BootstrapStatus::NotInitialized;
};

} // namespace VulkanEngine::Runtime
