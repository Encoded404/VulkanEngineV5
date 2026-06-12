module;

#include <SDL3/SDL_video.h>

// logging_macros.hpp has no <memory> include, safe in GMF.
#include <logging/logging_macros.hpp>

export module VulkanEngine.Application;

import std;
import logiface;

constexpr std::uint32_t UINT32_MAX =
    std::numeric_limits<std::uint32_t>::max();

import VulkanBackend.Event;
import VulkanBackend.Platform.SdlPlatform;
import VulkanBackend.Platform.SdlPlatformBackend;
import VulkanBackend.Runtime.FrameLoop;
import VulkanBackend.Utils.CallbackList;
import VulkanBackend.Utils.Timer;
import VulkanEngine.Input;
import VulkanBackend.Runtime.VulkanBootstrap;
import VulkanBackend.Runtime.VulkanBootstrapBackend;
import VulkanEngine.Startup;

export namespace VulkanEngine::Application {

struct ApplicationFrameState {
    VulkanEngine::Runtime::RuntimeFrameInfo runtime_frame{}; // NOLINT(misc-non-private-member-variables-in-classes)
    std::uint32_t image_index = 0; // NOLINT(misc-non-private-member-variables-in-classes)
    float delta_time = 0.0f; // NOLINT(misc-non-private-member-variables-in-classes)
    bool render_success = true; // NOLINT(misc-non-private-member-variables-in-classes)
};

struct ApplicationContext {
    VulkanEngine::Platform::SdlPlatform* platform = nullptr; // NOLINT(misc-non-private-member-variables-in-classes)
    VulkanEngine::Runtime::FrameLoop* runtime = nullptr; // NOLINT(misc-non-private-member-variables-in-classes)
    VulkanEngine::Runtime::VulkanBootstrap* bootstrap = nullptr; // NOLINT(misc-non-private-member-variables-in-classes)
    VulkanEngine::Input::InputSystem* input_system = nullptr; // NOLINT(misc-non-private-member-variables-in-classes)
    SDL_Window* window = nullptr; // NOLINT(misc-non-private-member-variables-in-classes)
    const VulkanEngine::Platform::PlatformState* platform_state = nullptr; // NOLINT(misc-non-private-member-variables-in-classes)
    ApplicationFrameState frame{}; // NOLINT(misc-non-private-member-variables-in-classes)
    VulkanEngine::Input::ActionHandle quit_action_handle{}; // NOLINT(misc-non-private-member-variables-in-classes)
    std::uint64_t geometry_buffer_size_mb = 128; // NOLINT(misc-non-private-member-variables-in-classes)
};

struct ApplicationConfig {
    std::string app_name = "VulkanEngineV5"; // NOLINT(misc-non-private-member-variables-in-classes)
    std::string log_level = "info"; // NOLINT(misc-non-private-member-variables-in-classes)
    VulkanEngine::Platform::PlatformConfig platform_config{}; // NOLINT(misc-non-private-member-variables-in-classes)
    VulkanEngine::Runtime::RuntimeConfig runtime_config{}; // NOLINT(misc-non-private-member-variables-in-classes)
    VulkanEngine::Runtime::VulkanBootstrapConfig bootstrap_config{}; // NOLINT(misc-non-private-member-variables-in-classes)
    std::uint32_t minimized_sleep_ms = 10; // NOLINT(misc-non-private-member-variables-in-classes)
    std::uint64_t geometry_buffer_size_mb = 128; // NOLINT(misc-non-private-member-variables-in-classes)
};

struct ApplicationHooks {
    Utils::CallbackList<bool(ApplicationContext&)> on_setup{}; // NOLINT(misc-non-private-member-variables-in-classes)
    Utils::CallbackList<void(ApplicationContext&)> on_pre_input{}; // NOLINT(misc-non-private-member-variables-in-classes)
    std::function<bool()> should_filter_mouse_input{}; // NOLINT(misc-non-private-member-variables-in-classes)
    std::function<bool()> should_filter_keyboard_input{}; // NOLINT(misc-non-private-member-variables-in-classes)
    Utils::OrderedCallbackList<void(ApplicationContext&)> on_frame_update{}; // NOLINT(misc-non-private-member-variables-in-classes)
    Utils::OrderedCallbackList<void(ApplicationContext&)> on_frame_render{}; // NOLINT(misc-non-private-member-variables-in-classes)
    Utils::CallbackList<void(ApplicationContext&)> on_shutdown{}; // NOLINT(misc-non-private-member-variables-in-classes)
};

[[nodiscard]] std::string_view PlatformStatusToString(VulkanEngine::Platform::PlatformStatus status) {
    using VulkanEngine::Platform::PlatformStatus;
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

// NOLINTBEGIN
[[nodiscard]] int RunApplication(const ApplicationConfig& config, const ApplicationHooks& hooks) {
    std::unique_ptr<VulkanEngine::Platform::SdlPlatform> platform{};
    std::shared_ptr<VulkanEngine::Runtime::IVulkanBootstrap> vk_backend{};
    std::unique_ptr<VulkanEngine::Runtime::VulkanBootstrap> bootstrap{};
    std::unique_ptr<VulkanEngine::Runtime::FrameLoop> runtime{};
    VulkanEngine::Input::InputSystem input_system{};
    ApplicationContext context{};
    bool platform_initialized = false;
    bool bootstrap_initialized = false;
    bool runtime_initialized = false;
    bool setup_completed = false;

    auto cleanup = [&]() {
        VulkanEngine::Utils::Timer t{true};
        double prev = 0.0;
        if (setup_completed) {
            hooks.on_shutdown.Call(context);
            const double current = t.ElapsedMs();
            LOGIFACE_LOG(debug, "shutdown: app hooks " + std::to_string(current - prev) + " ms");
            prev = current;
            setup_completed = false;
        }
        if (runtime_initialized && runtime) {
            runtime->Shutdown();
            const double current = t.ElapsedMs();
            LOGIFACE_LOG(debug, "shutdown: runtime " + std::to_string(current - prev) + " ms");
            prev = current;
            runtime_initialized = false;
        }
        if (bootstrap_initialized && bootstrap) {
            bootstrap->Shutdown();
            const double current = t.ElapsedMs();
            LOGIFACE_LOG(debug, "shutdown: bootstrap " + std::to_string(current - prev) + " ms");
            prev = current;
            bootstrap_initialized = false;
        }
        if (platform_initialized && platform) {
            platform->Shutdown();
            const double current = t.ElapsedMs();
            LOGIFACE_LOG(debug, "shutdown: platform " + std::to_string(current - prev) + " ms");
            prev = current;
            platform_initialized = false;
        }
        LOGIFACE_LOG(info, "shutdown: total " + std::to_string(t.ElapsedMs()) + " ms");
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
        platform = std::make_unique<VulkanEngine::Platform::SdlPlatform>(platform_backend);

        auto platform_config = config.platform_config;
        if (platform_config.window_title.empty()) {
            platform_config.window_title = config.app_name;
        }
        if (!platform->Initialize(platform_config)) {
            std::string msg = std::string{"Platform initialization failed: "} + std::string{PlatformStatusToString(platform->GetState().status)};
            if (!platform->GetState().error_message.empty()) {
                msg += " (" + platform->GetState().error_message + ")";
            }
            return fail(msg);
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

        runtime = std::make_unique<VulkanEngine::Runtime::FrameLoop>();
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
        context.geometry_buffer_size_mb = config.geometry_buffer_size_mb;

        if (!hooks.on_setup.Call(context)) {
            return fail("Application setup failed");
        }
        setup_completed = true;

        auto previous_time = std::chrono::steady_clock::now();

        while (!platform->ShouldQuit() && !runtime->ShouldShutdown()) {
            auto platform_events = platform->PollEvents();

            hooks.on_pre_input.Call(context);

            bool filter_mouse = hooks.should_filter_mouse_input && hooks.should_filter_mouse_input();
            bool filter_keyboard = hooks.should_filter_keyboard_input && hooks.should_filter_keyboard_input();

            VulkanEngine::Backend::Event::EventList filtered_events;
            filtered_events.reserve(platform_events.size());

            for (auto& event : platform_events) {
                const auto etype = event->GetEventType();
                bool is_mouse = etype == VulkanEngine::Backend::Event::EventType::MouseButtonDown ||
                                etype == VulkanEngine::Backend::Event::EventType::MouseButtonUp ||
                                etype == VulkanEngine::Backend::Event::EventType::MouseMotion ||
                                etype == VulkanEngine::Backend::Event::EventType::MouseWheel;

                bool is_keyboard = etype == VulkanEngine::Backend::Event::EventType::KeyDown ||
                                   etype == VulkanEngine::Backend::Event::EventType::KeyUp;

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

            hooks.on_frame_update.Call(context);

            context.frame.render_success = true;
            hooks.on_frame_render.Call(context);

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
        LOGIFACE_LOG(error, std::string("Fatal error: ") + ex.what());
        const auto trace = std::stacktrace::from_current_exception();
        std::cerr << "\nStacktrace:\n" << trace << '\n';
        cleanup();
        return 1;
    }
}
// NOLINTEND

} // namespace VulkanEngine::Application






