module;

#include <memory>
#include <string>

#include <SDL3/SDL_video.h>

export module VulkanBackend.Platform.SdlPlatform;

import VulkanBackend.Event;

export namespace VulkanEngine::Platform {

enum class PlatformStatus : uint8_t {
    Ok,
    NotInitialized,
    BackendInitFailed,
    WindowCreateFailed,
    QuitRequested,
    FatalError
};

struct PlatformConfig {
    std::string window_title = "VulkanEngineV5"; // NOLINT(misc-non-private-member-variables-in-classes)
    uint32_t window_width = 1280; // NOLINT(misc-non-private-member-variables-in-classes)
    uint32_t window_height = 720; // NOLINT(misc-non-private-member-variables-in-classes)
};

struct PlatformState {
    bool initialized = false; // NOLINT(misc-non-private-member-variables-in-classes)
    bool window_created = false; // NOLINT(misc-non-private-member-variables-in-classes)
    bool quit_requested = false; // NOLINT(misc-non-private-member-variables-in-classes)
    bool minimized = false; // NOLINT(misc-non-private-member-variables-in-classes)
    bool resized = false; // NOLINT(misc-non-private-member-variables-in-classes)
    uint32_t drawable_width = 0; // NOLINT(misc-non-private-member-variables-in-classes)
    uint32_t drawable_height = 0; // NOLINT(misc-non-private-member-variables-in-classes)
    PlatformStatus status = PlatformStatus::NotInitialized; // NOLINT(misc-non-private-member-variables-in-classes)
};

class IPlatformBackend {
public:
    virtual ~IPlatformBackend() = default;

    [[nodiscard]] virtual bool Initialize() = 0;
    virtual void Shutdown() = 0;
    [[nodiscard]] virtual bool CreateMainWindow(const PlatformConfig& config) = 0;
    [[nodiscard]] virtual VulkanEngine::Backend::Event::EventList PumpEvents() = 0;
    [[nodiscard]] virtual SDL_Window* GetNativeWindowHandle() const = 0;
};

class SdlPlatformShell {
public:
    explicit SdlPlatformShell(std::shared_ptr<IPlatformBackend> backend);

    [[nodiscard]] bool Initialize(const PlatformConfig& config);
    void Shutdown();
    [[nodiscard]] VulkanEngine::Backend::Event::EventList PollEvents();

    [[nodiscard]] const PlatformState& GetState() const;
    [[nodiscard]] bool IsInitialized() const;
    [[nodiscard]] bool ShouldQuit() const;
    [[nodiscard]] SDL_Window* GetNativeWindowHandle() const;

private:
    std::shared_ptr<IPlatformBackend> backend_{};
    PlatformState state_{};
};

}  // namespace VulkanEngine::Platform

