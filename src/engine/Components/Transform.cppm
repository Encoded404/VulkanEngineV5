module;

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/transform.hpp>

export module VulkanEngine.Components.Transform;

import VulkanEngine.Component;

export namespace App::Components {

class Transform : public VulkanEngine::Component {
public:
    VulkanEngine::FieldHandle<glm::vec3> position; // NOLINT(misc-non-private-member-variables-in-classes)
    VulkanEngine::FieldHandle<glm::quat> rotation; // NOLINT(misc-non-private-member-variables-in-classes)
    VulkanEngine::FieldHandle<glm::vec3> scale;    // NOLINT(misc-non-private-member-variables-in-classes)

    [[nodiscard]] glm::mat4 GetLocalMatrix() const {
        glm::mat4 matrix = glm::translate(glm::mat4(1.0f), *position);
        matrix *= glm::mat4_cast(*rotation);
        matrix = glm::scale(matrix, *scale);
        return matrix;
    }

    static auto GetFields() {
        return VulkanEngine::make_fields(
            VulkanEngine::field<glm::vec3>("position"),
            VulkanEngine::field<glm::quat>("rotation"),
            VulkanEngine::field<glm::vec3>("scale")
        );
    }
};

} // namespace App::Components
