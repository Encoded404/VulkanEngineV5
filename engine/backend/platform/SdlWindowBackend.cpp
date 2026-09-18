module;

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_video.h>


module VulkanBackend.Platform.SdlPlatformBackend;

import std;

import VulkanBackend.Event;
import VulkanShared.CallbackList;

namespace VulkanBackend::Platform {

namespace {

class SdlPlatformBackend final : public IPlatformBackend {
public:
    [[nodiscard]] bool Initialize() override {
        if (initialized_) {
            return true;
        }

        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
            return false;
        }

        initialized_ = true;
        return true;
    }

    void Shutdown() override {
        CloseAllGamepads();

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

    VulkanShared::CallbackList<void(void*)>& GetSdlEventProcessors() override {
        return sdl_event_processors_;
    }

    [[nodiscard]] VulkanBackend::Event::EventList PumpEvents() override {
        VulkanBackend::Event::EventList events{};

        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            sdl_event_processors_.Call(&event);

            switch (event.type) {
                case SDL_EVENT_QUIT:
                    events.push_back(std::make_unique<VulkanBackend::Event::QuitEvent>());
                    break;
                case SDL_EVENT_WINDOW_RESIZED:
                    events.push_back(std::make_unique<VulkanBackend::Event::WindowResizedEvent>(
                        static_cast<std::uint32_t>(event.window.data1),
                        static_cast<std::uint32_t>(event.window.data2)));
                    break;
                case SDL_EVENT_WINDOW_MINIMIZED:
                    events.push_back(std::make_unique<VulkanBackend::Event::WindowMinimizedEvent>());
                    break;
                case SDL_EVENT_WINDOW_RESTORED:
                    events.push_back(std::make_unique<VulkanBackend::Event::WindowRestoredEvent>());
                    break;
                case SDL_EVENT_KEY_DOWN:
                    events.push_back(std::make_unique<VulkanBackend::Event::KeyDownEvent>(
                        static_cast<std::int32_t>(event.key.key),
                        event.key.repeat != 0));
                    break;
                case SDL_EVENT_KEY_UP:
                    events.push_back(std::make_unique<VulkanBackend::Event::KeyUpEvent>(
                        static_cast<std::int32_t>(event.key.key)));
                    break;
                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                    events.push_back(std::make_unique<VulkanBackend::Event::MouseButtonDownEvent>(
                        static_cast<std::int32_t>(event.button.button),
                        static_cast<std::int32_t>(event.button.x),
                        static_cast<std::int32_t>(event.button.y)));
                    break;
                case SDL_EVENT_MOUSE_BUTTON_UP:
                    events.push_back(std::make_unique<VulkanBackend::Event::MouseButtonUpEvent>(
                        static_cast<std::int32_t>(event.button.button),
                        static_cast<std::int32_t>(event.button.x),
                        static_cast<std::int32_t>(event.button.y)));
                    break;
                case SDL_EVENT_MOUSE_MOTION:
                    events.push_back(std::make_unique<VulkanBackend::Event::MouseMotionEvent>(
                        static_cast<float>(event.motion.x),
                        static_cast<float>(event.motion.y),
                        static_cast<float>(event.motion.xrel),
                        static_cast<float>(event.motion.yrel)));
                    break;
                case SDL_EVENT_MOUSE_WHEEL:
                    events.push_back(std::make_unique<VulkanBackend::Event::MouseWheelEvent>(
                        static_cast<float>(event.wheel.x),
                        static_cast<float>(event.wheel.y)));
                    break;
                case SDL_EVENT_GAMEPAD_ADDED:
                    if (OpenGamepad(event.gdevice.which)) {
                        events.push_back(std::make_unique<VulkanBackend::Event::GamepadConnectedEvent>(
                            gamepad_indices_.at(event.gdevice.which)));
                    }
                    break;
                case SDL_EVENT_GAMEPAD_REMOVED:
                    if (std::optional<std::uint8_t> index = CloseGamepad(event.gdevice.which)) {
                        events.push_back(std::make_unique<VulkanBackend::Event::GamepadDisconnectedEvent>(*index));
                    }
                    break;
                case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                    if (std::optional<std::uint8_t> index = IndexForInstance(event.gbutton.which)) {
                        events.push_back(std::make_unique<VulkanBackend::Event::GamepadButtonDownEvent>(
                            *index, static_cast<std::int32_t>(event.gbutton.button)));
                    }
                    break;
                case SDL_EVENT_GAMEPAD_BUTTON_UP:
                    if (std::optional<std::uint8_t> index = IndexForInstance(event.gbutton.which)) {
                        events.push_back(std::make_unique<VulkanBackend::Event::GamepadButtonUpEvent>(
                            *index, static_cast<std::int32_t>(event.gbutton.button)));
                    }
                    break;
                case SDL_EVENT_GAMEPAD_AXIS_MOTION:
                    if (std::optional<std::uint8_t> index = IndexForInstance(event.gaxis.which)) {
                        events.push_back(std::make_unique<VulkanBackend::Event::GamepadAxisMotionEvent>(
                            *index,
                            static_cast<std::int32_t>(event.gaxis.axis),
                            static_cast<float>(event.gaxis.value) / 32767.0F));
                    }
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
    [[nodiscard]] std::optional<std::uint8_t> IndexForInstance(SDL_JoystickID instance_id) const {
        const auto it = gamepad_indices_.find(instance_id);
        if (it == gamepad_indices_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    [[nodiscard]] std::uint8_t AcquireGamepadIndex() const {
        for (std::uint8_t index = 0; index < 255; ++index) {
            if (!gamepads_.contains(index)) {
                return index;
            }
        }
        return 0;
    }

    bool OpenGamepad(SDL_JoystickID instance_id) {
        if (gamepad_indices_.contains(instance_id)) {
            return false;
        }

        SDL_Gamepad* gamepad = SDL_OpenGamepad(instance_id);
        if (gamepad == nullptr) {
            return false;
        }

        const std::uint8_t index = AcquireGamepadIndex();
        gamepads_[index] = gamepad;
        gamepad_indices_[instance_id] = index;
        return true;
    }

    std::optional<std::uint8_t> CloseGamepad(SDL_JoystickID instance_id) {
        const auto it = gamepad_indices_.find(instance_id);
        if (it == gamepad_indices_.end()) {
            return std::nullopt;
        }

        const std::uint8_t index = it->second;
        if (const auto gamepad_it = gamepads_.find(index); gamepad_it != gamepads_.end()) {
            SDL_CloseGamepad(gamepad_it->second);
            gamepads_.erase(gamepad_it);
        }
        gamepad_indices_.erase(it);
        return index;
    }

    void CloseAllGamepads() {
        for (auto& [gamepad_index, gamepad] : gamepads_) {
            static_cast<void>(gamepad_index);
            SDL_CloseGamepad(gamepad);
        }
        gamepads_.clear();
        gamepad_indices_.clear();
    }

    SDL_Window* window_ = nullptr;
    bool initialized_ = false;
    std::unordered_map<SDL_JoystickID, std::uint8_t> gamepad_indices_{};
    std::unordered_map<std::uint8_t, SDL_Gamepad*> gamepads_{};
    VulkanShared::CallbackList<void(void*)> sdl_event_processors_{};
};

}  // namespace

std::shared_ptr<IPlatformBackend> CreateSdlPlatformBackend() {
    return std::make_shared<SdlPlatformBackend>();
}

}  // namespace VulkanBackend::Platform



