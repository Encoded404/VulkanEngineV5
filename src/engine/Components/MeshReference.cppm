module;

#include <cstdint>

export module VulkanEngine.Components.MeshReference;

import VulkanBackend.Component;

export namespace VulkanEngine::Components {

class MeshReference : public VulkanEngine::Component {
public:
    uint32_t vertex_offset = 0; // NOLINT(misc-non-private-member-variables-in-classes)
    uint32_t vertex_count = 0;  // NOLINT(misc-non-private-member-variables-in-classes)
    uint32_t index_offset = 0;  // NOLINT(misc-non-private-member-variables-in-classes)
    uint32_t index_count = 0;   // NOLINT(misc-non-private-member-variables-in-classes)

    static auto GetFields() {
        return VulkanEngine::make_fields(
            VulkanEngine::field<uint32_t>("vertex_offset"),
            VulkanEngine::field<uint32_t>("vertex_count"),
            VulkanEngine::field<uint32_t>("index_offset"),
            VulkanEngine::field<uint32_t>("index_count")
        );
    }
};

}
