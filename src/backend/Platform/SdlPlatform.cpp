module;

#include <memory>
#include <utility>

#include <SDL3/SDL_video.h>

module VulkanBackend.Platform.SdlPlatform;

namespace VulkanEngine::Platform {

SdlPlatformShell::SdlPlatformShell(std::shared_ptr<IPlatformBackend> backend)
    : backend_(std::move(backend)) {}

bool SdlPlatformShell::Initialize(const PlatformConfig& config) {
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
        return false;
    }

    state_.window_created = true;
    state_.drawable_width = config.window_width;
    state_.drawable_height = config.window_height;
    return true;
}

void SdlPlatformShell::Shutdown() {
    if (backend_ && state_.initialized) {
        backend_->Shutdown();
    }

    state_ = PlatformState{};
}

void SdlPlatformShell::PollEvents() {
    if (!state_.initialized) {
        state_.status = PlatformStatus::NotInitialized;
        return;
    }

    state_.resized = false;

    for (const auto& event : backend_->PumpEvents()) {
        switch (event.type) {
            case PlatformEventType::Quit:
                state_.quit_requested = true;
                state_.status = PlatformStatus::QuitRequested;
                break;
            case PlatformEventType::WindowResized:
                state_.resized = true;
                state_.minimized = false;
                state_.drawable_width = event.width;
                state_.drawable_height = event.height;
                state_.status = PlatformStatus::Ok;
                break;
            case PlatformEventType::WindowMinimized:
                state_.minimized = true;
                state_.status = PlatformStatus::Ok;
                break;
            case PlatformEventType::WindowRestored:
                state_.minimized = false;
                state_.status = PlatformStatus::Ok;
                break;
            case PlatformEventType::None:
            default:
                break;
        }
    }
}

const PlatformState& SdlPlatformShell::GetState() const {
    return state_;
}

bool SdlPlatformShell::IsInitialized() const {
    return state_.initialized;
}

bool SdlPlatformShell::ShouldQuit() const {
    return state_.quit_requested;
}

SDL_Window* SdlPlatformShell::GetNativeWindowHandle() const {
    if (!backend_) {
        return nullptr;
    }
    return backend_->GetNativeWindowHandle();
}

}  // namespace VulkanEngine::Platform



