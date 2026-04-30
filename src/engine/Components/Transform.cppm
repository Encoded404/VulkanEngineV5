module;

#include <glm/glm.hpp> // NOLINT(misc-include-cleaner)

export module VulkanEngine.Components.Transform;

import VulkanBackend.Component;

export namespace App::Components {

class Transform : public VulkanEngine::Component {
public:
    glm::vec3 position{0.0f, 0.0f, 0.0f}; // NOLINT(misc-non-private-member-variables-in-classes)
    float rotation_degrees_y = 0.0f;       // NOLINT(misc-non-private-member-variables-in-classes)
    glm::vec3 scale{1.0f, 1.0f, 1.0f};     // NOLINT(misc-non-private-member-variables-in-classes)

    static auto GetFields() {
        return VulkanEngine::make_fields(
            VulkanEngine::field<glm::vec3>("position"),
            VulkanEngine::field<float>("rotation_degrees_y"),
            VulkanEngine::field<glm::vec3>("scale")
        );
    }
};

} // namespace App::Components
