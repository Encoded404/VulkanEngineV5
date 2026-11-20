module;

export module VulkanEngine.Components.MeshReference;

import std;

import VulkanEngine.ECS.ComponentRegistry;
import VulkanEngine.MeshHandle;

export namespace VulkanEngine::Components {

// Data component: which mesh asset this entity draws. Dense storage.
struct MeshReference {
    // The short, stable name Describe() prints for this target. A plain
    // static member does not affect the object/data split, which is
    // is_base_of_v<Component, T>.
    static constexpr std::string_view kName = "MeshReference";
    // The mesh this entity draws, or an invalid handle while it draws nothing.
    MeshHandle loaded_mesh_id{}; // NOLINT(misc-non-private-member-variables-in-classes)
};

} // namespace VulkanEngine::Components
