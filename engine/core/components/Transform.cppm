module;

#include <glm/glm.hpp> // NOLINT(misc-include-cleaner)
#include <glm/gtc/quaternion.hpp> // NOLINT(misc-include-cleaner)

export module VulkanEngine.Components.Transform;

import std;

import VulkanEngine.ECS.ComponentRegistry;

export namespace VulkanEngine::Components {

// Data component: dense storage, no behavior. Defaults are member initializers,
// so a freshly added Transform is usable immediately (no lazy Initialize).
struct Transform {
    // The short, stable name Describe() prints for this target. A plain
    // static member does not affect the object/data split, which is
    // is_base_of_v<Component, T>.
    static constexpr std::string_view kName = "Transform";
    glm::vec3 position{0.0f, 0.0f, 0.0f}; // NOLINT(misc-non-private-member-variables-in-classes)
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f}; // NOLINT(misc-non-private-member-variables-in-classes)
    glm::vec3 scale{1.0f, 1.0f, 1.0f}; // NOLINT(misc-non-private-member-variables-in-classes)
};

} // namespace VulkanEngine::Components
