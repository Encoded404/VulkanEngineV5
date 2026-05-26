module;

#include <cstdint>

export module VulkanEngine.Components.MeshReference;

import VulkanBackend.Component;

export namespace VulkanEngine::Components {

class MeshReference : public VulkanEngine::Component {
public:
    uint32_t first_submesh = 0;    // NOLINT(misc-non-private-member-variables-in-classes)
    uint32_t submesh_count = 0;    // NOLINT(misc-non-private-member-variables-in-classes)
    uint8_t index_buffer_index = 0; // NOLINT(misc-non-private-member-variables-in-classes)

    static auto GetFields() {
        return VulkanEngine::make_fields(
            VulkanEngine::field<uint32_t>("first_submesh"),
            VulkanEngine::field<uint32_t>("submesh_count"),
            VulkanEngine::field<uint8_t>("index_buffer_index")
        );
    }
};

}
