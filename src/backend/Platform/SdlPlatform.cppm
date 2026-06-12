module;

#include <SDL3/SDL_video.h>

export module VulkanBackend.Platform.SdlPlatform;

import std;

import VulkanBackend.Event;
import VulkanBackend.Utils.CallbackList;

export namespace VulkanEngine::Platform {

enum class PlatformStatus : std::uint8_t {
    Ok,
    NotInitialized,
    BackendInitFailed,
    WindowCreateFailed,
    QuitRequested,
    FatalError
};

struct PlatformConfig {
    std::string window_title = "VulkanEngineV5"; // NOLINT(misc-non-private-member-variables-in-classes)
    std::uint32_t window_width = 1280; // NOLINT(misc-non-private-member-variables-in-classes)
    std::uint32_t window_height = 720; // NOLINT(misc-non-private-member-variables-in-classes)
};

struct PlatformState {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    bool initialized = false;
    bool window_created = false;
    bool quit_requested = false;
    bool minimized = false;
    bool resized = false;
    std::uint32_t drawable_width = 0;
    std::uint32_t drawable_height = 0;
    PlatformStatus status = PlatformStatus::NotInitialized;
    std::string error_message; // Optional detailed error message for fatal errors
    // NOLINTEND(misc-non-private-member-variables-in-classes)
};

class IPlatformBackend {
public:
    virtual ~IPlatformBackend() = default;

    [[nodiscard]] virtual bool Initialize() = 0;
    virtual void Shutdown() = 0;
    [[nodiscard]] virtual bool CreateMainWindow(const PlatformConfig& config) = 0;
    [[nodiscard]] virtual VulkanEngine::Backend::Event::EventList PumpEvents() = 0;
    [[nodiscard]] virtual SDL_Window* GetNativeWindowHandle() const = 0;

    virtual Utils::CallbackList<void(void*)>& GetSdlEventProcessors() = 0;
};

class SdlPlatform {
public:
    explicit SdlPlatform(std::shared_ptr<IPlatformBackend> backend);

    [[nodiscard]] bool Initialize(const PlatformConfig& config);
    void Shutdown();
    [[nodiscard]] VulkanEngine::Backend::Event::EventList PollEvents();

    [[nodiscard]] const PlatformState& GetState() const;
    [[nodiscard]] bool IsInitialized() const;
    [[nodiscard]] bool ShouldQuit() const;
    [[nodiscard]] SDL_Window* GetNativeWindowHandle() const;
    [[nodiscard]] IPlatformBackend& GetBackend() const;

private:
    std::shared_ptr<IPlatformBackend> backend_{};
    PlatformState state_{};
};

}  // namespace VulkanEngine::Platform

