module;

#include <glm/glm.hpp> // NOLINT(misc-include-cleaner)

export module VulkanEngine.Components.OrmOverride;

import std;

import VulkanEngine.ECS.ComponentRegistry;

export namespace VulkanEngine::Components {

// Per-object ORM override. When present on an entity with a MeshReference,
// MeshRenderSystem packs these values into the per-submesh StaticEntry and
// the fragment shader multiplies them with the material's ORM (texture or
// factors). Without this component, the material's own factors are used.
// All values are clamped to [0, 1] when packed (8-bit per channel).
class OrmOverride : public VulkanEngine::Component {
public:
    float ao = 1.0f;        // NOLINT(misc-non-private-member-variables-in-classes)
    float roughness = 1.0f; // NOLINT(misc-non-private-member-variables-in-classes)
    float metallic = 1.0f;  // NOLINT(misc-non-private-member-variables-in-classes)

    static auto GetFields() {
        return VulkanEngine::make_fields(
            VulkanEngine::field<float>("ao"),
            VulkanEngine::field<float>("roughness"),
            VulkanEngine::field<float>("metallic")
        );
    }
};

} // namespace VulkanEngine::Components
