module;

#include <SDL3/SDL_video.h>

#include <logging/logging_macros.hpp>

export module VulkanEngine.Application;

import std;

import logiface;

import VulkanBackend.Event;
import VulkanBackend.Platform.SdlPlatform;
import VulkanBackend.Vulkan.FrameLoop;
import VulkanShared.CallbackList;
import VulkanEngine.Input;
import VulkanBackend.Vulkan.VulkanBootstrap;

export namespace VulkanEngine::Application {

// Presentation cadence for the frame loop.
//
// Sequential: every rendered frame is presented immediately after its command
//   buffer is submitted (classic immediate loop).
//
// DiscardStale: frames are submitted as fast as the CPU can produce them and
//   presented only once their GPU work has actually completed (fence-gated
//   present queue). When the CPU outproduces the GPU, older presented frames are
//   dropped by the swapchain compositor in Mailbox mode and the freshest frame
//   wins at VBlank — the "render ahead, present latest" behavior. Requires
//   frames_in_flight >= 3 and a Mailbox/Immediate present mode.
enum class PresentPolicy : std::uint8_t {
    Sequential,
    DiscardStale,
};

struct ApplicationFrameState {
    VulkanBackend::Vulkan::RuntimeFrameInfo runtime_frame{}; // NOLINT(misc-non-private-member-variables-in-classes)
    std::uint32_t frame_counter = 0; // NOLINT(misc-non-private-member-variables-in-classes)
    std::uint32_t image_index = 0; // NOLINT(misc-non-private-member-variables-in-classes)
    float delta_time = 0.0f; // NOLINT(misc-non-private-member-variables-in-classes)
    bool render_success = true; // NOLINT(misc-non-private-member-variables-in-classes)
};

struct ApplicationContext {
    VulkanBackend::Platform::SdlPlatform* platform = nullptr; // NOLINT(misc-non-private-member-variables-in-classes)
    VulkanBackend::Vulkan::FrameLoop* runtime = nullptr; // NOLINT(misc-non-private-member-variables-in-classes)
    VulkanBackend::Vulkan::VulkanBootstrap* bootstrap = nullptr; // NOLINT(misc-non-private-member-variables-in-classes)
    VulkanEngine::Input::InputSystem* input_system = nullptr; // NOLINT(misc-non-private-member-variables-in-classes)
    SDL_Window* window = nullptr; // NOLINT(misc-non-private-member-variables-in-classes)
    const VulkanBackend::Platform::PlatformState* platform_state = nullptr; // NOLINT(misc-non-private-member-variables-in-classes)
    ApplicationFrameState frame{}; // NOLINT(misc-non-private-member-variables-in-classes)
    VulkanEngine::Input::ActionHandle quit_action_handle{}; // NOLINT(misc-non-private-member-variables-in-classes)
    std::uint64_t geometry_buffer_size_mb = 128; // NOLINT(misc-non-private-member-variables-in-classes)
};

struct ApplicationConfig {
    std::string app_name = "VulkanEngineV5"; // NOLINT(misc-non-private-member-variables-in-classes)
    std::string log_level = "info"; // NOLINT(misc-non-private-member-variables-in-classes)
    // ── Frame pipeline (single source of truth) ──
    // Number of frames that may be in flight (CPU slots running ahead of the GPU).
    // All engine rings, the device sync structures, and the pipeline depth are
    // sized from this one value. Only 2 or 3 make practical sense; 3 enables
    // PresentPolicy::DiscardStale.
    std::uint32_t frames_in_flight = 3; // NOLINT(misc-non-private-member-variables-in-classes)
    // Swapchain image count. 0 = derive from present mode + frames_in_flight
    // (FIFO -> frames_in_flight + 1, Mailbox/Immediate -> max(frames_in_flight, 2),
    // DiscardStale -> frames_in_flight + 1). Values are clamped to the driver's
    // min/max image counts during swapchain creation.
    std::uint32_t swapchain_image_count = 0; // NOLINT(misc-non-private-member-variables-in-classes)
    PresentPolicy present_policy = PresentPolicy::Sequential; // NOLINT(misc-non-private-member-variables-in-classes)
    VulkanBackend::Platform::PlatformConfig platform_config{}; // NOLINT(misc-non-private-member-variables-in-classes)
    VulkanBackend::Vulkan::RuntimeConfig runtime_config{}; // NOLINT(misc-non-private-member-variables-in-classes)
    VulkanBackend::Vulkan::VulkanBootstrapConfig bootstrap_config{}; // NOLINT(misc-non-private-member-variables-in-classes)
    std::uint32_t minimized_sleep_ms = 10; // NOLINT(misc-non-private-member-variables-in-classes)
    std::uint64_t geometry_buffer_size_mb = 128; // NOLINT(misc-non-private-member-variables-in-classes)
};

struct ApplicationHooks {
    VulkanShared::CallbackList<bool(ApplicationContext&)> on_setup{}; // NOLINT(misc-non-private-member-variables-in-classes)
    VulkanShared::CallbackList<void(ApplicationContext&)> on_pre_input{}; // NOLINT(misc-non-private-member-variables-in-classes)
    std::function<bool()> should_filter_mouse_input{}; // NOLINT(misc-non-private-member-variables-in-classes)
    std::function<bool()> should_filter_keyboard_input{}; // NOLINT(misc-non-private-member-variables-in-classes)
    VulkanShared::OrderedCallbackList<void(ApplicationContext&)> on_frame_update{}; // NOLINT(misc-non-private-member-variables-in-classes)
    VulkanShared::OrderedCallbackList<void(ApplicationContext&)> on_frame_render{}; // NOLINT(misc-non-private-member-variables-in-classes)
    VulkanShared::CallbackList<void(ApplicationContext&)> on_shutdown{}; // NOLINT(misc-non-private-member-variables-in-classes)
};

[[nodiscard]] inline std::string_view PlatformStatusToString(VulkanBackend::Platform::PlatformStatus status) {
    using VulkanBackend::Platform::PlatformStatus;
    switch (status) {
        case PlatformStatus::Ok: return "Ok";
        case PlatformStatus::NotInitialized: return "NotInitialized";
        case PlatformStatus::BackendInitFailed: return "BackendInitFailed";
        case PlatformStatus::WindowCreateFailed: return "WindowCreateFailed";
        case PlatformStatus::QuitRequested: return "QuitRequested";
        case PlatformStatus::FatalError: return "FatalError";
    }
    return "unknown";
}

} // namespace VulkanEngine::Application
