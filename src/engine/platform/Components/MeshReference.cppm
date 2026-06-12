module;

export module VulkanEngine.Components.MeshReference;

import std;

import VulkanBackend.Component;

constexpr std::uint32_t UINT32_MAX =
    std::numeric_limits<std::uint32_t>::max();


export namespace VulkanEngine::Components {

class MeshReference : public VulkanEngine::Component {
public:
    std::uint32_t loaded_mesh_id = UINT32_MAX; // NOLINT(misc-non-private-member-variables-in-classes)

    static auto GetFields() {
        return VulkanEngine::make_fields(
            VulkanEngine::field<std::uint32_t>("loaded_mesh_id")
        );
    }
};

}
