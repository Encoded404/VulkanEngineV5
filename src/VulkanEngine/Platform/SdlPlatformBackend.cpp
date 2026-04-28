module;

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_video.h>

#include <memory>
#include <vector>

module VulkanEngine.Platform.SdlPlatformBackend;

namespace VulkanEngine::Platform {

namespace {

class SdlPlatformBackend final : public IPlatformBackend {
public:
    [[nodiscard]] bool Initialize() override {
        if (initialized_) {
            return true;
        }

        if (!SDL_Init(SDL_INIT_VIDEO)) {
            return false;
        }

        initialized_ = true;
        return true;
    }

    void Shutdown() override {
        if (window_ != nullptr) {
            SDL_DestroyWindow(window_);
            window_ = nullptr;
        }

        if (initialized_) {
            SDL_Quit();
            initialized_ = false;
        }
    }

    [[nodiscard]] bool CreateMainWindow(const PlatformConfig& config) override {
        if (!initialized_) {
            return false;
        }

        if (window_ != nullptr) {
            SDL_DestroyWindow(window_);
            window_ = nullptr;
        }

        window_ = SDL_CreateWindow(
            config.window_title.c_str(),
            static_cast<int>(config.window_width),
            static_cast<int>(config.window_height),
            SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);

        return window_ != nullptr;
    }

    [[nodiscard]] std::vector<PlatformEvent> PumpEvents() override {
        std::vector<PlatformEvent> events{};

        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_EVENT_QUIT:
                    events.push_back(PlatformEvent{.type = PlatformEventType::Quit});
                    break;
                case SDL_EVENT_WINDOW_RESIZED:
                    events.push_back(PlatformEvent{
                        .type = PlatformEventType::WindowResized,
                        .width = static_cast<uint32_t>(event.window.data1),
                        .height = static_cast<uint32_t>(event.window.data2),
                    });
                    break;
                case SDL_EVENT_WINDOW_MINIMIZED:
                    events.push_back(PlatformEvent{.type = PlatformEventType::WindowMinimized});
                    break;
                case SDL_EVENT_WINDOW_RESTORED:
                    events.push_back(PlatformEvent{.type = PlatformEventType::WindowRestored});
                    break;
                default:
                    break;
            }
        }

        return events;
    }

    [[nodiscard]] SDL_Window* GetNativeWindowHandle() const override {
        return window_;
    }

private:
    SDL_Window* window_ = nullptr;
    bool initialized_ = false;
};

}  // namespace

std::shared_ptr<IPlatformBackend> CreateSdlPlatformBackend() {
    return std::make_shared<SdlPlatformBackend>();
}

}  // namespace VulkanEngine::Platform




