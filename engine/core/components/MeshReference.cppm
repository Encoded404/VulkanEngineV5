module;

export module VulkanEngine.Components.MeshReference;

import std;

import VulkanEngine.ECS.ComponentRegistry;

export namespace VulkanEngine::Components {

// Data component: which mesh asset this entity draws. Dense storage.
struct MeshReference {
    std::uint32_t loaded_mesh_id = std::numeric_limits<std::uint32_t>::max(); // NOLINT(misc-non-private-member-variables-in-classes)
};

} // namespace VulkanEngine::Components
