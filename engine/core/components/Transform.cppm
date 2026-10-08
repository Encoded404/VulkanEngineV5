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
    glm::vec3 position{0.0f, 0.0f, 0.0f}; // NOLINT(misc-non-private-member-variables-in-classes)
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f}; // NOLINT(misc-non-private-member-variables-in-classes)
    glm::vec3 scale{1.0f, 1.0f, 1.0f}; // NOLINT(misc-non-private-member-variables-in-classes)
};

} // namespace VulkanEngine::Components
