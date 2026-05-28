module;

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_RADIANS
#include <glm/glm.hpp> // NOLINT(misc-include-cleaner)
#include <glm/gtc/quaternion.hpp> // NOLINT(misc-include-cleaner)

export module VulkanEngine.Components.Transform;

import VulkanBackend.Component;

export namespace VulkanEngine::Components {

class Transform : public VulkanEngine::Component {
public:
    VulkanEngine::FieldHandle<glm::vec3> position; // NOLINT(misc-non-private-member-variables-in-classes)
    VulkanEngine::FieldHandle<glm::quat> rotation; // NOLINT(misc-non-private-member-variables-in-classes)
    VulkanEngine::FieldHandle<glm::vec3> scale;    // NOLINT(misc-non-private-member-variables-in-classes)

    void Initialize() override {
        position = glm::vec3{0.0f, 0.0f, 0.0f};
        scale = glm::vec3{1.0f, 1.0f, 1.0f};
        rotation = glm::quat{1.0f, 0.0f, 0.0f, 0.0f};
    }

    static auto GetFields() {
        return VulkanEngine::make_fields(
            VulkanEngine::field<glm::vec3>("position"),
            VulkanEngine::field<glm::quat>("rotation"),
            VulkanEngine::field<glm::vec3>("scale")
        );
    }

    auto GetFieldHandles() {
        return std::forward_as_tuple(position, rotation, scale);
    }
};

} // namespace VulkanEngine::Components
