module;

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_RADIANS
#include <glm/glm.hpp> //NOLINT(misc-include-cleaner)
#include <glm/gtc/matrix_transform.hpp> //NOLINT(misc-include-cleaner)

export module VulkanEngine.Components.Camera;

export import VulkanBackend.Component;

export namespace VulkanEngine::Components {

enum class ProjectionMode : uint8_t {
    Perspective,
    Orthographic
};

class Camera : public VulkanEngine::Component {
public:
    glm::vec3 position{0.0f, 0.0f, 3.0f}; // NOLINT(misc-non-private-member-variables-in-classes)
    glm::vec3 target{0.0f, 0.0f, 0.0f}; // NOLINT(misc-non-private-member-variables-in-classes)
    glm::vec3 up{0.0f, 1.0f, 0.0f}; // NOLINT(misc-non-private-member-variables-in-classes)
    float fov_degrees = 60.0f; // NOLINT(misc-non-private-member-variables-in-classes)
    float orthographic_size = 10.0f; // NOLINT(misc-non-private-member-variables-in-classes)
    float near_plane = 0.1f; // NOLINT(misc-non-private-member-variables-in-classes)
    float far_plane = 100.0f; // NOLINT(misc-non-private-member-variables-in-classes)
    ProjectionMode projection_mode = ProjectionMode::Perspective; // NOLINT(misc-non-private-member-variables-in-classes)

    [[nodiscard]] glm::mat4 GetViewMatrix() const {
        return glm::lookAt(position, target, up);
    }

    [[nodiscard]] glm::mat4 GetProjectionMatrix(float aspect) const {
        if (projection_mode == ProjectionMode::Orthographic) {
            const float half_h = orthographic_size * 0.5f;
            const float half_w = half_h * aspect;
            return glm::ortho(-half_w, half_w, -half_h, half_h, near_plane, far_plane);
        }
        const float fov_rad = fov_degrees * (std::numbers::pi_v<float> / 180.0f);
        return glm::perspective(fov_rad, aspect, near_plane, far_plane);
    }

    static auto GetFields() {
        return VulkanEngine::make_fields(
            VulkanEngine::field<glm::vec3>("position"),
            VulkanEngine::field<glm::vec3>("target"),
            VulkanEngine::field<glm::vec3>("up"),
            VulkanEngine::field<float>("fov_degrees"),
            VulkanEngine::field<float>("orthographic_size"),
            VulkanEngine::field<float>("near_plane"),
            VulkanEngine::field<float>("far_plane"),
            VulkanEngine::field<uint8_t>("projection_mode")
        );
    }
};

}
