module;

#include <SDL3/SDL_video.h>

#include <logging/logging_macros.hpp>

export module Runtime.Application;

import std;

import logiface;

import VulkanBackend.Event;
import VulkanBackend.Platform.SdlPlatform;
import VulkanBackend.Platform.SdlPlatformBackend;
import VulkanBackend.Vulkan.FrameLoop;
import VulkanShared.CallbackList;
import VulkanShared.Timer;
import VulkanEngine.Input;
import VulkanBackend.Vulkan.VulkanBootstrap;
import VulkanBackend.Vulkan.VulkanBootstrapBackend;
import VulkanEngine.Startup;
import VulkanEngine.Application;

#ifndef UINT32_MAX
constexpr std::uint32_t UINT32_MAX =
    std::numeric_limits<std::uint32_t>::max();
#endif

export namespace VulkanEngine::Application {

// NOLINTBEGIN
[[nodiscard]] inline int RunApplication(const ApplicationConfig& config, const ApplicationHooks& hooks) {
    std::unique_ptr<VulkanBackend::Platform::SdlPlatform> platform{};
    std::shared_ptr<VulkanBackend::Vulkan::IVulkanBootstrap> vk_backend{};
    std::unique_ptr<VulkanBackend::Vulkan::VulkanBootstrap> bootstrap{};
    std::unique_ptr<VulkanBackend::Vulkan::FrameLoop> runtime{};
    VulkanEngine::Input::InputSystem input_system{};
    ApplicationContext context{};
    bool platform_initialized = false;
    bool bootstrap_initialized = false;
    bool runtime_initialized = false;
    bool setup_completed = false;
    // DiscardStale: renders submitted to the queue but not yet presented, in
    // frame order. Presents are issued only once a frame's GPU work completes,
    // so the Mailbox compositor always sees (and scans out) the freshest frames.
    struct PendingPresent { std::uint32_t frame_counter; std::uint32_t image_index; };
    std::vector<PendingPresent> pending_presents{};
    const bool discard_stale = config.present_policy == PresentPolicy::DiscardStale;

    auto cleanup = [&]() {
        VulkanShared::Timer t{true};
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

        const auto platform_backend = VulkanBackend::Platform::CreateSdlPlatformBackend();
        platform = std::make_unique<VulkanBackend::Platform::SdlPlatform>(platform_backend);

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

        vk_backend = VulkanBackend::Vulkan::CreateVulkanBootstrapBackend();
        bootstrap = std::make_unique<VulkanBackend::Vulkan::VulkanBootstrap>(vk_backend);
        auto bootstrap_config = config.bootstrap_config;

        // ── Frame pipeline is configured from the single ApplicationConfig ──
        // frame-clock: the device sync rings (command buffers, fences, semaphores)
        // and every engine FIF ring are sized from this value.
        const std::uint32_t fif = std::clamp(config.frames_in_flight, 1u, 4u);
        if (config.frames_in_flight != fif || config.frames_in_flight < 2) {
            LOGIFACE_LOG(warn, "frames_in_flight=" + std::to_string(config.frames_in_flight) +
                         " clamped to " + std::to_string(fif) + "; only 2 or 3 make practical sense");
        }
        if (discard_stale &&
            (fif < 3 ||
             (bootstrap_config.present_mode != VulkanBackend::Vulkan::PresentMode::Mailbox &&
              bootstrap_config.present_mode != VulkanBackend::Vulkan::PresentMode::Immediate))) {
            return fail("PresentPolicy::DiscardStale requires frames_in_flight >= 3 and a "
                        "Mailbox or Immediate present mode (stale-frame discard is performed by the "
                        "swapchain compositor)");
        }
        bootstrap_config.frames_in_flight = fif;

        if (config.swapchain_image_count >= 2) {
            bootstrap_config.preferred_swapchain_image_count = config.swapchain_image_count;
        } else {
            // Derive from present mode: with FIFO the present queue holds an image
            // until its VBlank turn, so it cannot be re-acquired meanwhile; Mailbox
            // drops pending presents, so images become free as soon as the render
            // completes.
            const bool fifo_present =
                bootstrap_config.present_mode == VulkanBackend::Vulkan::PresentMode::Fifo ||
                bootstrap_config.present_mode == VulkanBackend::Vulkan::PresentMode::FifoRelaxed;
            bootstrap_config.preferred_swapchain_image_count =
                fifo_present ? fif + 1u : std::max(fif, 2u);
        }
        if (discard_stale) {
            // Deferred presents can hold up to FIF-1 images while the GPU catches
            // up; budget one spare so acquire never stalls on image reuse.
            bootstrap_config.preferred_swapchain_image_count =
                std::max(bootstrap_config.preferred_swapchain_image_count, fif + 1u);
        }
        bootstrap_config.native_window_handle = window;
        if (!bootstrap->Initialize(bootstrap_config)) {
            std::string bootstrap_message = "Vulkan bootstrap initialization failed";
            if (!bootstrap->GetSnapshot().error_message.empty()) {
                bootstrap_message += ": " + bootstrap->GetSnapshot().error_message;
            }
            return fail(bootstrap_message);
        }
        bootstrap_initialized = true;

        runtime = std::make_unique<VulkanBackend::Vulkan::FrameLoop>();
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

            VulkanBackend::Event::EventList filtered_events;
            filtered_events.reserve(platform_events.size());

            for (auto& event : platform_events) {
                const auto etype = event->GetEventType();
                bool is_mouse = etype == VulkanBackend::Event::EventType::MouseButtonDown ||
                                etype == VulkanBackend::Event::EventType::MouseButtonUp ||
                                etype == VulkanBackend::Event::EventType::MouseMotion ||
                                etype == VulkanBackend::Event::EventType::MouseWheel;

                bool is_keyboard = etype == VulkanBackend::Event::EventType::KeyDown ||
                                   etype == VulkanBackend::Event::EventType::KeyUp;

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
            if (context.frame.runtime_frame.status == VulkanBackend::Vulkan::RuntimeStatus::Minimized ||
                context.frame.runtime_frame.status == VulkanBackend::Vulkan::RuntimeStatus::ShutdownRequested) {
                std::this_thread::sleep_for(std::chrono::milliseconds(config.minimized_sleep_ms));
                continue;
            }

            if (context.platform_state->resized) {
                bootstrap->NotifySwapchainOutOfDate();
            }

            // DiscardStale: release frames whose GPU work has completed. Presents
            // are issued in submission order; the Mailbox compositor then scans out
            // the freshest completed frame and drops the stale ones.
            if (discard_stale) {
                while (!pending_presents.empty()) {
                    PendingPresent& front = pending_presents.front();
                    // The front frame's slot fence reflects exactly its own work: no
                    // later frame has reused the slot (that reuse is gated on this
                    // same fence by AcquireNextImage), so a signaled fence means the
                    // frame itself is GPU-complete.
                    if (!bootstrap->IsFrameComplete(front.frame_counter)) {
                        break;
                    }
                    if (!bootstrap->Present(front.image_index)) {
                        bootstrap->NotifySwapchainOutOfDate();
                        break;
                    }
                    pending_presents.erase(pending_presents.begin());
                }
            }

            const auto bootstrap_frame = bootstrap->BeginFrame();
            if (bootstrap_frame.status == VulkanBackend::Vulkan::BootstrapStatus::SwapchainOutOfDate) {
                if (!bootstrap->RecreateSwapchain()) {
                    return fail("Swapchain recreation failed");
                }
                continue;
            }
            if (bootstrap_frame.status != VulkanBackend::Vulkan::BootstrapStatus::Ok) {
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

            context.frame.frame_counter = bootstrap->GetSnapshot().frame_index;

            hooks.on_frame_update.Call(context);

            context.frame.render_success = true;
            hooks.on_frame_render.Call(context);

            if (discard_stale) {
                // Submit now, present later (once the GPU work completes — see the
                // drain at the top of the loop). Frames whose present is superseded
                // are dropped by the Mailbox compositor; every image is still
                // presented exactly once, so none leaks.
                if (!bootstrap->SubmitFrame(context.frame.image_index,
                                            context.frame.render_success)) {
                    bootstrap->NotifySwapchainOutOfDate();
                } else if (context.frame.render_success) {
                    pending_presents.push_back(
                        {context.frame.frame_counter, context.frame.image_index});
                }
            } else if (!bootstrap->SubmitFrame(context.frame.image_index,
                                               context.frame.render_success)) {
                bootstrap->NotifySwapchainOutOfDate();
            } else if (context.frame.render_success &&
                       !bootstrap->Present(context.frame.image_index)) {
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
        cleanup();
        return 1;
    }
}
// NOLINTEND

} // namespace VulkanEngine::Application
