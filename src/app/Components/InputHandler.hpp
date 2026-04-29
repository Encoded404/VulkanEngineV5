#pragma once

import VulkanEngine.Component;
import VulkanEngine.Input;

#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_mouse.h>

namespace App::Components {

class InputHandler : public VulkanEngine::Component {
public:
    explicit InputHandler(VulkanEngine::Input::InputSystem* input_system) noexcept
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

        constexpr float move_speed = 1.5f;
        if (input_system_->IsActionActive("move_left")) {
            monkey_offset_x_ -= move_speed * delta_time;
        }
        if (input_system_->IsActionActive("move_right")) {
            monkey_offset_x_ += move_speed * delta_time;
        }
        if (input_system_->IsActionActive("move_up")) {
            monkey_offset_y_ += move_speed * delta_time;
        }
        if (input_system_->IsActionActive("move_down")) {
            monkey_offset_y_ -= move_speed * delta_time;
        }

        if (!input_system_->IsActionActive("pause_spin")) {
            angle_ += delta_time * 90.0f;
            if (angle_ >= 360.0f) angle_ -= 360.0f;
        }
    }

    float GetAngle() const noexcept { return angle_; }
    float GetMonkeyOffsetX() const noexcept { return monkey_offset_x_; }
    float GetMonkeyOffsetY() const noexcept { return monkey_offset_y_; }

private:
    VulkanEngine::Input::InputSystem* input_system_ = nullptr; // not owned
    float angle_ = 0.0f;
    float monkey_offset_x_ = 0.0f;
    float monkey_offset_y_ = 0.0f;
};

} // namespace App::Components

