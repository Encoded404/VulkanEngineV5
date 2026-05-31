module;

#include <cstdint>

export module VulkanEngine.Components.MeshReference;

import VulkanBackend.Component;

export namespace VulkanEngine::Components {

class MeshReference : public VulkanEngine::Component {
public:
    uint32_t loaded_mesh_id = UINT32_MAX; // NOLINT(misc-non-private-member-variables-in-classes)

    static auto GetFields() {
        return VulkanEngine::make_fields(
            VulkanEngine::field<uint32_t>("loaded_mesh_id")
        );
    }
};

}
