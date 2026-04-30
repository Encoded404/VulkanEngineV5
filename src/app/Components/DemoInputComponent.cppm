module;

#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_mouse.h>

export module App.Components.DemoInputComponent;

import VulkanBackend.Component;
import VulkanEngine.Input;
import VulkanEngine.Components.Transform;

export namespace App::Components {

class DemoInputComponent : public VulkanEngine::Component {
public:
    explicit DemoInputComponent(VulkanEngine::Input::InputSystem* input_system) noexcept
        : input_system_(input_system) {}

    void Initialize() override {
        if (!input_system_) return;
        // Bind the actions the app expects
        input_system_->BindAction("quit", VulkanEngine::Input::InputBinding::Key(SDLK_ESCAPE));
        input_system_->BindAction("move_left", VulkanEngine::Input::InputBinding::Key(SDLK_A));
        input_system_->BindAction("move_right", VulkanEngine::Input::InputBinding::Key(SDLK_D));
        input_system_->BindAction("move_up", VulkanEngine::Input::InputBinding::Key(SDLK_W));
        input_system_->BindAction("move_down", VulkanEngine::Input::InputBinding::Key(SDLK_S));
        input_system_->BindAction("pause_spin", VulkanEngine::Input::InputBinding::MouseButton(SDL_BUTTON_LEFT));
    }

    void Update(float delta_time) override {
        if (!input_system_) return;

        auto* transform = GetOwner() != nullptr ? GetOwner()->GetComponent<App::Components::Transform>() : nullptr;
        if (transform == nullptr) {
            return;
        }

        constexpr float move_speed = 1.5f;
        if (input_system_->IsActionActive("move_left")) {
            transform->position.x -= move_speed * delta_time;
        }
        if (input_system_->IsActionActive("move_right")) {
            transform->position.x += move_speed * delta_time;
        }
        if (input_system_->IsActionActive("move_up")) {
            transform->position.y += move_speed * delta_time;
        }
        if (input_system_->IsActionActive("move_down")) {
            transform->position.y -= move_speed * delta_time;
        }

        if (!input_system_->IsActionActive("pause_spin")) {
            transform->rotation_degrees_y += delta_time * 90.0f;
            if (transform->rotation_degrees_y >= 360.0f) {
                transform->rotation_degrees_y -= 360.0f;
            }
        }
    }

private:
    VulkanEngine::Input::InputSystem* input_system_ = nullptr; // not owned
};

} // namespace App::Components



