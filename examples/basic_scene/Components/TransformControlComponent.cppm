module;

#include <glm/glm.hpp> //NOLINT(misc-include-cleaner)
#include <glm/gtc/quaternion.hpp> //NOLINT(misc-include-cleaner)

export module Examples.BasicScene.Components.TransformControlComponent;

import std;

import VulkanEngine.ECS.ComponentRegistry;
import VulkanEngine.Components.Transform;

export namespace Examples::BasicScene::Components {

enum class RotationMode : std::uint8_t {
    Euler,
    Quaternion
};

// Object component: a debug-panel-controlled transform driver. Stored as a
// stable object, so examples may hold a pointer to it across frames.
//
// It writes only its own entity's Transform, touches no cross-entity component
// and no service, so `SelfParallel` is the honest declaration: instances on
// different entities never share state.
class TransformControlComponent : public VulkanEngine::Component {
public:
    using SelfWrites = VulkanEngine::AccessSet<VulkanEngine::Components::Transform>;
    static constexpr VulkanEngine::Concurrency kConcurrency =
        VulkanEngine::Concurrency::SelfParallel;
    static constexpr std::string_view kName = "examples.transform_control";

    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    glm::vec3 position{0.0f, 0.0f, 0.0f};
    RotationMode rotation_mode = RotationMode::Euler;
    glm::vec3 rotation_euler{0.0f, 0.0f, 0.0f};
    glm::quat rotation_quat{1.0f, 0.0f, 0.0f, 0.0f};
    int texture_slot = 0;
    // NOLINTEND(misc-non-private-member-variables-in-classes)

    void Update(float /*delta_time*/, VulkanEngine::UpdateScope& scope) override {
        auto* transform = scope.Find<VulkanEngine::Components::Transform>();
        if (transform) {
            transform->position = position;
            if (rotation_mode == RotationMode::Euler) {
                transform->rotation = glm::quat{glm::radians(rotation_euler)};
            } else {
                transform->rotation = rotation_quat;
            }
        }
    }
};

}
