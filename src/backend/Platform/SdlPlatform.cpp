module;

#include <memory>
#include <utility>

#include <SDL3/SDL_video.h>

module VulkanBackend.Platform.SdlPlatform;

import VulkanBackend.Event;

namespace VulkanEngine::Platform {

SdlPlatform::SdlPlatform(std::shared_ptr<IPlatformBackend> backend)
    : backend_(std::move(backend)) {}

bool SdlPlatform::Initialize(const PlatformConfig& config) {
    if (!backend_) {
        state_.status = PlatformStatus::FatalError;
        return false;
    }

    if (!backend_->Initialize()) {
        state_.status = PlatformStatus::BackendInitFailed;
        state_.initialized = false;
        return false;
    }

    state_.initialized = true;
    state_.status = PlatformStatus::Ok;

    if (!backend_->CreateMainWindow(config)) {
        state_.status = PlatformStatus::WindowCreateFailed;
        state_.window_created = false;
        state_.error_message = "Failed to create main window: " + std::string(SDL_GetError());
        return false;
    }

    state_.window_created = true;
    state_.drawable_width = config.window_width;
    state_.drawable_height = config.window_height;
    return true;
}

void SdlPlatform::Shutdown() {
    if (backend_ && state_.initialized) {
        backend_->Shutdown();
    }

    state_ = PlatformState{};
}

VulkanEngine::Backend::Event::EventList SdlPlatform::PollEvents() {
    if (!state_.initialized) {
        state_.status = PlatformStatus::NotInitialized;
        return {};
    }

    state_.resized = false;

    auto events = backend_->PumpEvents();

    for (const auto& event : events) {
        if (dynamic_cast<const VulkanEngine::Backend::Event::QuitEvent*>(event.get()) != nullptr) {
            state_.quit_requested = true;
            state_.status = PlatformStatus::QuitRequested;
            continue;
        }

        if (const auto* resized_event = dynamic_cast<const VulkanEngine::Backend::Event::WindowResizedEvent*>(event.get()); resized_event != nullptr) {
            state_.resized = true;
            state_.minimized = false;
            state_.drawable_width = resized_event->width;
            state_.drawable_height = resized_event->height;
            state_.status = PlatformStatus::Ok;
            continue;
        }

        if (dynamic_cast<const VulkanEngine::Backend::Event::WindowMinimizedEvent*>(event.get()) != nullptr) {
            state_.minimized = true;
            state_.status = PlatformStatus::Ok;
            continue;
        }

        if (dynamic_cast<const VulkanEngine::Backend::Event::WindowRestoredEvent*>(event.get()) != nullptr) {
            state_.minimized = false;
            state_.status = PlatformStatus::Ok;
        }
    }

    return events;
}

const PlatformState& SdlPlatform::GetState() const {
    return state_;
}

bool SdlPlatform::IsInitialized() const {
    return state_.initialized;
}

bool SdlPlatform::ShouldQuit() const {
    return state_.quit_requested;
}

SDL_Window* SdlPlatform::GetNativeWindowHandle() const {
    if (!backend_) {
        return nullptr;
    }
    return backend_->GetNativeWindowHandle();
}

IPlatformBackend& SdlPlatform::GetBackend() const {
    return *backend_;
}

}  // namespace VulkanEngine::Platform



