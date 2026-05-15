module;

#include <functional>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_video.h>

#include <memory>
#include <vector>

module VulkanEngine.Platform.SdlPlatformBackend;

import VulkanBackend.Event;

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

    void SetEventProcessor(EventProcessor processor, void* user_data) override {
        event_processor_ = processor;
        event_processor_user_data_ = user_data;
    }

    [[nodiscard]] VulkanEngine::Backend::Event::EventList PumpEvents() override {
        VulkanEngine::Backend::Event::EventList events{};

        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            if (event_processor_) {
                event_processor_(event_processor_user_data_, &event);
            }
            
            switch (event.type) {
                case SDL_EVENT_QUIT:
                    events.push_back(std::make_unique<VulkanEngine::Backend::Event::QuitEvent>());
                    break;
                case SDL_EVENT_WINDOW_RESIZED:
                    events.push_back(std::make_unique<VulkanEngine::Backend::Event::WindowResizedEvent>(
                        static_cast<uint32_t>(event.window.data1),
                        static_cast<uint32_t>(event.window.data2)));
                    break;
                case SDL_EVENT_WINDOW_MINIMIZED:
                    events.push_back(std::make_unique<VulkanEngine::Backend::Event::WindowMinimizedEvent>());
                    break;
                case SDL_EVENT_WINDOW_RESTORED:
                    events.push_back(std::make_unique<VulkanEngine::Backend::Event::WindowRestoredEvent>());
                    break;
                case SDL_EVENT_KEY_DOWN:
                    events.push_back(std::make_unique<VulkanEngine::Backend::Event::KeyDownEvent>(
                        static_cast<int32_t>(event.key.key),
                        event.key.repeat != 0));
                    break;
                case SDL_EVENT_KEY_UP:
                    events.push_back(std::make_unique<VulkanEngine::Backend::Event::KeyUpEvent>(
                        static_cast<int32_t>(event.key.key)));
                    break;
                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                    events.push_back(std::make_unique<VulkanEngine::Backend::Event::MouseButtonDownEvent>(
                        static_cast<int32_t>(event.button.button),
                        static_cast<int32_t>(event.button.x),
                        static_cast<int32_t>(event.button.y)));
                    break;
                case SDL_EVENT_MOUSE_BUTTON_UP:
                    events.push_back(std::make_unique<VulkanEngine::Backend::Event::MouseButtonUpEvent>(
                        static_cast<int32_t>(event.button.button),
                        static_cast<int32_t>(event.button.x),
                        static_cast<int32_t>(event.button.y)));
                    break;
                case SDL_EVENT_MOUSE_MOTION:
                    events.push_back(std::make_unique<VulkanEngine::Backend::Event::MouseMotionEvent>(
                        static_cast<float>(event.motion.x),
                        static_cast<float>(event.motion.y),
                        static_cast<float>(event.motion.xrel),
                        static_cast<float>(event.motion.yrel)));
                    break;
                case SDL_EVENT_MOUSE_WHEEL:
                    events.push_back(std::make_unique<VulkanEngine::Backend::Event::MouseWheelEvent>(
                        static_cast<float>(event.wheel.x),
                        static_cast<float>(event.wheel.y)));
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
    EventProcessor event_processor_ = nullptr;
    void* event_processor_user_data_ = nullptr;
};

}  // namespace

std::shared_ptr<IPlatformBackend> CreateSdlPlatformBackend() {
    return std::make_shared<SdlPlatformBackend>();
}

}  // namespace VulkanEngine::Platform




