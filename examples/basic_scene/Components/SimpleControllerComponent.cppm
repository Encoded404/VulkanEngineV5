module;

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp> // NOLINT(misc-include-cleaner)
#include <glm/gtc/quaternion.hpp> // NOLINT(misc-include-cleaner)
#include <glm/gtx/string_cast.hpp> // NOLINT(misc-include-cleaner)

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_mouse.h>

#include <logging/logging_macros.hpp>

export module Examples.BasicScene.Components.SimpleControllerComponent;

import std;
import logiface;


import VulkanEngine.ECS.ComponentRegistry;
import VulkanEngine.ECS.ComponentContext;
import VulkanEngine.Input;
import VulkanEngine.Components.Transform;

export namespace Examples::BasicScene::Components {

// Object component: owns input bindings and drives its entity's Transform.
//
// Declaration: read and write the same entity's Transform, read the input
// service. `SelfParallel` is legal because the type touches no cross-entity
// component and holds no writable service bit: instances on different entities
// are disjoint by construction, which is exactly what the mode promises.
//
// The action binding is a one-time service *write*, so it lives in OnConfigure
// (serial, before any level); Update only reads input. That is what keeps the
// mutating BindAction call off the parallel path.
class SimpleControllerComponent : public VulkanEngine::Component {
public:
    using SelfReads = VulkanEngine::AccessSet<VulkanEngine::Components::Transform>;
    using SelfWrites = VulkanEngine::AccessSet<VulkanEngine::Components::Transform>;
    static constexpr VulkanEngine::Concurrency kConcurrency =
        VulkanEngine::Concurrency::SelfParallel;
    static constexpr std::uint32_t kServices =
        VulkanEngine::ServiceRead(VulkanEngine::Service::Input);
    static constexpr std::string_view kName = "examples.simple_controller";

    SimpleControllerComponent() noexcept = default;

    void OnConfigure(const VulkanEngine::ComponentContext& context) override {
        auto* input_system = context.Input();
        if (input_system == nullptr) return;
        BindActions(*input_system);
    }

    void Update(float delta_time, VulkanEngine::UpdateScope& scope) override {
        const auto* input_system = scope.Services().InputRead();
        if (input_system == nullptr) return;

        auto* transform = scope.Find<VulkanEngine::Components::Transform>();
        if (transform == nullptr) return;

        constexpr float move_speed = 1.5f;
        const auto move = move_.Value();
        transform->position.x += move[0] * move_speed * delta_time;
        transform->position.y += move[1] * move_speed * delta_time;

        if (!input_system->IsActionActive(pause_spin_handle_)) {
            const auto yaw = glm::angleAxis(glm::radians(delta_time * 90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
            LOGIFACE_LOG(trace, "Applying rotation: " + glm::to_string(glm::eulerAngles(yaw)) + " degrees");
#ifndef NDEBUG
            const auto old_q = transform->rotation;  // raw quat before
            const auto current_euler = glm::eulerAngles(transform->rotation);
#endif
            transform->rotation = yaw * transform->rotation;
#ifndef NDEBUG
            const auto new_euler = glm::eulerAngles(transform->rotation);
            LOGIFACE_LOG(trace, "New rotation: " + glm::to_string(new_euler) + " degrees (was " + glm::to_string(current_euler) + ")");
            const auto new_q = transform->rotation;  // raw quat after
            LOGIFACE_LOG(trace, "Old q: " + glm::to_string(old_q) + " New q: " + glm::to_string(new_q) + " yaw: " + glm::to_string(yaw));
#endif
        }
    }

private:
    void BindActions(VulkanEngine::Input::InputSystem& input_system) {
        // One 2D action for both keyboard (WASD) and the gamepad's left stick.
        move_ = input_system.BindAction<2>("move", VulkanEngine::Input::ActionConfig{
            .processing = VulkanEngine::Input::ActionProcessing::ClampLength,
        });
        move_.Bind(VulkanEngine::Input::InputBinding::Key(SDLK_A).Component(0, -1.0f));
        move_.Bind(VulkanEngine::Input::InputBinding::Key(SDLK_D).Component(0, +1.0f));
        move_.Bind(VulkanEngine::Input::InputBinding::Key(SDLK_W).Component(1, +1.0f));
        move_.Bind(VulkanEngine::Input::InputBinding::Key(SDLK_S).Component(1, -1.0f));
        move_.BindGamepadStick(SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY);

        pause_spin_handle_ = input_system.BindAction("pause_spin", VulkanEngine::Input::InputBinding::MouseButton(SDL_BUTTON_LEFT));
    }

    VulkanEngine::Input::Action<2> move_{};
    VulkanEngine::Input::ActionHandle pause_spin_handle_;
};

}
