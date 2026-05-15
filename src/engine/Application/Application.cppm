module;

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <iostream>
#include <string>
#include <thread>

#include <SDL3/SDL_video.h>
#include <boost/stacktrace.hpp> // NOLINT(misc-include-cleaner)
#include <logging/logging.hpp>

export module VulkanEngine.Application;

import VulkanBackend.Event;
import VulkanBackend.Platform.SdlPlatform;
import VulkanEngine.Platform.SdlPlatformBackend;
import VulkanBackend.Runtime.FrameLoop;
import VulkanEngine.Input;
import VulkanEngine.Runtime.VulkanBootstrap;
import VulkanEngine.Runtime.VulkanBootstrapBackend;
import VulkanEngine.Startup;

export namespace VulkanEngine::Application {

struct ApplicationFrameState {
    VulkanEngine::Runtime::RuntimeFrameInfo runtime_frame{}; // NOLINT(misc-non-private-member-variables-in-classes)
    uint32_t image_index = 0; // NOLINT(misc-non-private-member-variables-in-classes)
    float delta_time = 0.0f; // NOLINT(misc-non-private-member-variables-in-classes)
    bool render_success = true; // NOLINT(misc-non-private-member-variables-in-classes)
};

struct ApplicationContext {
    VulkanEngine::Platform::SdlPlatformShell* platform = nullptr; // NOLINT(misc-non-private-member-variables-in-classes)
    VulkanEngine::Runtime::RuntimeShell* runtime = nullptr; // NOLINT(misc-non-private-member-variables-in-classes)
    VulkanEngine::Runtime::VulkanBootstrap* bootstrap = nullptr; // NOLINT(misc-non-private-member-variables-in-classes)
    VulkanEngine::Input::InputSystem* input_system = nullptr; // NOLINT(misc-non-private-member-variables-in-classes)
    SDL_Window* window = nullptr; // NOLINT(misc-non-private-member-variables-in-classes)
    const VulkanEngine::Platform::PlatformState* platform_state = nullptr; // NOLINT(misc-non-private-member-variables-in-classes)
    ApplicationFrameState frame{}; // NOLINT(misc-non-private-member-variables-in-classes)
    VulkanEngine::Input::ActionHandle quit_action_handle{}; // NOLINT(misc-non-private-member-variables-in-classes)
};

struct ApplicationConfig {
    std::string app_name = "VulkanEngineV5"; // NOLINT(misc-non-private-member-variables-in-classes)
    std::string log_level = "info"; // NOLINT(misc-non-private-member-variables-in-classes)
    VulkanEngine::Platform::PlatformConfig platform_config{}; // NOLINT(misc-non-private-member-variables-in-classes)
    VulkanEngine::Runtime::RuntimeConfig runtime_config{}; // NOLINT(misc-non-private-member-variables-in-classes)
    VulkanEngine::Runtime::VulkanBootstrapConfig bootstrap_config{}; // NOLINT(misc-non-private-member-variables-in-classes)
    uint32_t minimized_sleep_ms = 10; // NOLINT(misc-non-private-member-variables-in-classes)
};

struct ApplicationHooks {
    std::function<bool(ApplicationContext&)> on_setup{}; // NOLINT(misc-non-private-member-variables-in-classes)
    std::function<void(ApplicationContext&)> on_pre_input{}; // NOLINT(misc-non-private-member-variables-in-classes)
    std::function<bool()> should_filter_mouse_input{}; // NOLINT(misc-non-private-member-variables-in-classes)
    std::function<bool()> should_filter_keyboard_input{}; // NOLINT(misc-non-private-member-variables-in-classes)
    std::function<void(ApplicationContext&)> on_frame_update{}; // NOLINT(misc-non-private-member-variables-in-classes)
    std::function<void(ApplicationContext&)> on_frame_render{}; // NOLINT(misc-non-private-member-variables-in-classes)
    std::function<void(ApplicationContext&)> on_shutdown{}; // NOLINT(misc-non-private-member-variables-in-classes)
};

// NOLINTBEGIN
[[nodiscard]] int RunApplication(const ApplicationConfig& config, const ApplicationHooks& hooks) {
    std::unique_ptr<VulkanEngine::Platform::SdlPlatformShell> platform{};
    std::shared_ptr<VulkanEngine::Runtime::IVulkanBootstrapBackend> vk_backend{};
    std::unique_ptr<VulkanEngine::Runtime::VulkanBootstrap> bootstrap{};
    std::unique_ptr<VulkanEngine::Runtime::RuntimeShell> runtime{};
    VulkanEngine::Input::InputSystem input_system{};
    ApplicationContext context{};
    bool platform_initialized = false;
    bool bootstrap_initialized = false;
    bool runtime_initialized = false;
    bool setup_completed = false;

    auto cleanup = [&]() {
        if (setup_completed && hooks.on_shutdown) {
            hooks.on_shutdown(context);
            setup_completed = false;
        }
        if (runtime_initialized && runtime) {
            runtime->Shutdown();
            runtime_initialized = false;
        }
        if (bootstrap_initialized && bootstrap) {
            bootstrap->Shutdown();
            bootstrap_initialized = false;
        }
        if (platform_initialized && platform) {
            platform->Shutdown();
            platform_initialized = false;
        }
    };

    auto fail = [&](const std::string& message) -> int {
        LOGIFACE_LOG(error, message);
        cleanup();
        return 1;
    };

    try {
        VulkanEngine::Startup::InitializeLogger(config.log_level);
        LOGIFACE_LOG(info, config.app_name + " started");

        const auto platform_backend = VulkanEngine::Platform::CreateSdlPlatformBackend();
        platform = std::make_unique<VulkanEngine::Platform::SdlPlatformShell>(platform_backend);

        auto platform_config = config.platform_config;
        if (platform_config.window_title.empty()) {
            platform_config.window_title = config.app_name;
        }
        if (!platform->Initialize(platform_config)) {
            return fail("Platform initialization failed");
        }
        platform_initialized = true;

        SDL_Window* window = platform->GetNativeWindowHandle();
        if (window == nullptr) {
            return fail("Native SDL window handle is null");
        }

        vk_backend = VulkanEngine::Runtime::CreateVulkanBootstrapBackend();
        bootstrap = std::make_unique<VulkanEngine::Runtime::VulkanBootstrap>(vk_backend);
        auto bootstrap_config = config.bootstrap_config;
        bootstrap_config.native_window_handle = window;
        if (!bootstrap->Initialize(bootstrap_config)) {
            return fail("Vulkan bootstrap initialization failed");
        }
        bootstrap_initialized = true;

        runtime = std::make_unique<VulkanEngine::Runtime::RuntimeShell>();
        if (!runtime->Initialize(config.runtime_config)) {
            return fail("Runtime shell initialization failed");
        }
        runtime_initialized = true;

        context.platform = platform.get();
        context.runtime = runtime.get();
        context.bootstrap = bootstrap.get();
        context.input_system = &input_system;
        context.window = window;
        context.platform_state = &platform->GetState();

        if (hooks.on_setup && !hooks.on_setup(context)) {
            return fail("Application setup failed");
        }
        setup_completed = true;

        auto previous_time = std::chrono::steady_clock::now();

        while (!platform->ShouldQuit() && !runtime->ShouldShutdown()) {
            auto platform_events = platform->PollEvents();

            if (hooks.on_pre_input) {
                hooks.on_pre_input(context);
            }

            bool filter_mouse = hooks.should_filter_mouse_input && hooks.should_filter_mouse_input();
            bool filter_keyboard = hooks.should_filter_keyboard_input && hooks.should_filter_keyboard_input();

            VulkanEngine::Backend::Event::EventList filtered_events;
            filtered_events.reserve(platform_events.size());

            for (auto& event : platform_events) {
                bool is_mouse = dynamic_cast<const VulkanEngine::Backend::Event::MouseButtonDownEvent*>(event.get()) ||
                                dynamic_cast<const VulkanEngine::Backend::Event::MouseButtonUpEvent*>(event.get()) ||
                                dynamic_cast<const VulkanEngine::Backend::Event::MouseMotionEvent*>(event.get()) ||
                                dynamic_cast<const VulkanEngine::Backend::Event::MouseWheelEvent*>(event.get());

                bool is_keyboard = dynamic_cast<const VulkanEngine::Backend::Event::KeyDownEvent*>(event.get()) ||
                                   dynamic_cast<const VulkanEngine::Backend::Event::KeyUpEvent*>(event.get());

                if (is_mouse && filter_mouse) {
                    continue;
                }
                if (is_keyboard && filter_keyboard) {
                    continue;
                }
                filtered_events.push_back(std::move(event));
            }

            input_system.ProcessEvents(filtered_events);
            if (context.quit_action_handle.id != UINT32_MAX && input_system.WasActionStarted(context.quit_action_handle)) {
                runtime->RequestShutdown();
            }

            context.platform_state = &platform->GetState();
            if (context.platform_state->quit_requested) {
                runtime->RequestShutdown();
            }

            runtime->NotifyWindowMinimized(context.platform_state->minimized);
            context.frame.runtime_frame = runtime->BeginFrame();
            if (context.frame.runtime_frame.status == VulkanEngine::Runtime::RuntimeStatus::Minimized ||
                context.frame.runtime_frame.status == VulkanEngine::Runtime::RuntimeStatus::ShutdownRequested) {
                std::this_thread::sleep_for(std::chrono::milliseconds(config.minimized_sleep_ms));
                continue;
            }

            if (context.platform_state->resized) {
                bootstrap->NotifySwapchainOutOfDate();
            }

            const auto bootstrap_frame = bootstrap->BeginFrame();
            if (bootstrap_frame.status == VulkanEngine::Runtime::BootstrapStatus::SwapchainOutOfDate) {
                if (!bootstrap->RecreateSwapchain()) {
                    return fail("Swapchain recreation failed");
                }
                continue;
            }
            if (bootstrap_frame.status != VulkanEngine::Runtime::BootstrapStatus::Ok) {
                return fail("Vulkan bootstrap entered non-OK frame status");
            }

            context.frame.image_index = 0;
            if (!bootstrap->AcquireNextImage(context.frame.image_index)) {
                bootstrap->NotifySwapchainOutOfDate();
                continue;
            }

            const auto now = std::chrono::steady_clock::now();
            context.frame.delta_time = std::chrono::duration<float>(now - previous_time).count();
            previous_time = now;

            if (hooks.on_frame_update) {
                hooks.on_frame_update(context);
            }

            context.frame.render_success = true;
            if (hooks.on_frame_render) {
                hooks.on_frame_render(context);
            }

            if (!bootstrap->Present(context.frame.image_index, context.frame.render_success)) {
                bootstrap->NotifySwapchainOutOfDate();
            }

            runtime->EndFrame();
            bootstrap->EndFrame();
        }

        cleanup();
        LOGIFACE_LOG(info, "App completed");
        return 0;
    } catch (const std::exception& ex) {
        VulkanEngine::Startup::InitializeLogger(Logiface::Level::info);
        LOGIFACE_LOG(error, std::string("Fatal error: ") + ex.what());
        const auto trace = boost::stacktrace::stacktrace::from_current_exception();
        std::cerr << "\nStacktrace:\n" << trace << '\n';
        cleanup();
        return 1;
    }
}
// NOLINTEND

} // namespace VulkanEngine::Application






