module;

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_RADIANS
#include <glm/glm.hpp>

#include <cstdint>

export module VulkanEngine.Components.Material;

import VulkanBackend.Component;

export namespace VulkanEngine::Components {

class Material : public VulkanEngine::Component {
public:
    uint16_t technique_id = 0;       // NOLINT(misc-non-private-member-variables-in-classes)
    uint32_t bindless_texture_slot = 0; // NOLINT(misc-non-private-member-variables-in-classes)

    // Instance overrides (applied on top of base material)
    glm::vec4 color_tint{1.0f, 1.0f, 1.0f, 1.0f};       // NOLINT(misc-non-private-member-variables-in-classes)
    float roughness_offset = 0.0f;                        // NOLINT(misc-non-private-member-variables-in-classes)
    float metalness_offset = 0.0f;                        // NOLINT(misc-non-private-member-variables-in-classes)

    static auto GetFields() {
        return VulkanEngine::make_fields(
            VulkanEngine::field<uint16_t>("technique_id"),
            VulkanEngine::field<uint32_t>("bindless_texture_slot"),
            VulkanEngine::field<glm::vec4>("color_tint"),
            VulkanEngine::field<float>("roughness_offset"),
            VulkanEngine::field<float>("metalness_offset")
        );
    }
};

}
