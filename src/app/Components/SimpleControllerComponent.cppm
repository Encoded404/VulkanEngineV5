module;

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_RADIANS
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/quaternion.hpp> // NOLINT(misc-include-cleaner)
#include <glm/gtx/string_cast.hpp> // NOLINT(misc-include-cleaner)
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_mouse.h>

// logging_macros.hpp has no <memory> include, safe in GMF.
#include <logging/logging_macros.hpp>

export module App.Components.SimpleControllerComponent;

import std;
import logiface;


import VulkanBackend.Component;
import VulkanEngine.Input;
import VulkanEngine.Components.Transform;

export namespace App::Components {

class SimpleControllerComponent : public VulkanEngine::Component {
public:
    explicit SimpleControllerComponent(VulkanEngine::Input::InputSystem* input_system) noexcept
        : input_system_(input_system) {}

    void Initialize() override {
        if (!input_system_) return;
        move_left_handle_ = input_system_->BindAction("move_left", VulkanEngine::Input::InputBinding::Key(SDLK_A));
        move_right_handle_ = input_system_->BindAction("move_right", VulkanEngine::Input::InputBinding::Key(SDLK_D));
        move_up_handle_ = input_system_->BindAction("move_up", VulkanEngine::Input::InputBinding::Key(SDLK_W));
        move_down_handle_ = input_system_->BindAction("move_down", VulkanEngine::Input::InputBinding::Key(SDLK_S));
        pause_spin_handle_ = input_system_->BindAction("pause_spin", VulkanEngine::Input::InputBinding::MouseButton(SDL_BUTTON_LEFT));
    }

    void Update(float delta_time) override {
        if (!input_system_) return;

        auto* transform = GetOwner() != nullptr ? GetOwner()->GetComponent<VulkanEngine::Components::Transform>() : nullptr;
        if (transform == nullptr) {
            return;
        }

        constexpr float move_speed = 1.5f;
        if (input_system_->IsActionActive(move_left_handle_)) {
            transform->position->x -= move_speed * delta_time;
        }
        if (input_system_->IsActionActive(move_right_handle_)) {
            transform->position->x += move_speed * delta_time;
        }
        if (input_system_->IsActionActive(move_up_handle_)) {
            transform->position->y += move_speed * delta_time;
        }
        if (input_system_->IsActionActive(move_down_handle_)) {
            transform->position->y -= move_speed * delta_time;
        }

        if (!input_system_->IsActionActive(pause_spin_handle_)) {
            const auto yaw = glm::angleAxis(glm::radians(delta_time * 90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
            LOGIFACE_LOG(trace, "Applying rotation: " + glm::to_string(glm::eulerAngles(yaw)) + " degrees");
#ifndef NDEBUG
            const auto old_q = *transform->rotation;  // raw quat before
            const auto current_euler = glm::eulerAngles(*transform->rotation);
#endif
            transform->rotation = yaw * *transform->rotation;
#ifndef NDEBUG
            const auto new_euler = glm::eulerAngles(*transform->rotation);
            LOGIFACE_LOG(trace, "New rotation: " + glm::to_string(new_euler) + " degrees (was " + glm::to_string(current_euler) + ")");
            const auto new_q = *transform->rotation;  // raw quat after
            LOGIFACE_LOG(trace, "Old q: " + glm::to_string(old_q) + " New q: " + glm::to_string(new_q) + " yaw: " + glm::to_string(yaw));
#endif
        }
    }

private:
    VulkanEngine::Input::InputSystem* input_system_ = nullptr;
    VulkanEngine::Input::ActionHandle move_left_handle_;
    VulkanEngine::Input::ActionHandle move_right_handle_;
    VulkanEngine::Input::ActionHandle move_up_handle_;
    VulkanEngine::Input::ActionHandle move_down_handle_;
    VulkanEngine::Input::ActionHandle pause_spin_handle_;
};

}
