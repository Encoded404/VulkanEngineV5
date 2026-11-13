module;

export module VulkanEngine.Components.DynamicMesh;

import std;

import VulkanEngine.ECS.ComponentRegistry;
import VulkanEngine.GpuResources.MeshData;
import VulkanEngine.MeshManager;

export namespace VulkanEngine::Components {

// Data component: a streamed mesh owned by this entity. Dense storage; the
// engine releases `gpu_handle` through a registry OnRemove hook, because the
// component does not own a MeshManager.
//
// Streamed meshes are vertex+index only. Their geometry is double-buffered per
// frame in flight, while a skin-influence table and a modifier stack are single
// resident resources the deform stage addresses by absolute element index, so a
// streamed mesh cannot carry either: `MeshManager::RegisterStreamed` drops
// `MeshData::influence_sets` (with one warning) and gather ignores a
// GeometryModifier on the entity. Both render through the plain vertex path.
struct DynamicMesh {
    // The short, stable name Describe() prints for this target. A plain
    // static member does not affect the object/data split, which is
    // is_base_of_v<Component, T>.
    static constexpr std::string_view kName = "DynamicMesh";
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    MeshManager::Handle gpu_handle;
    GpuResources::MeshData mesh_data;
    std::uint32_t first_submesh = 0;
    std::uint32_t submesh_count = 0;
    // NOLINTEND(misc-non-private-member-variables-in-classes)

    void SetupStreamed(MeshManager& mgr, const GpuResources::MeshData& initial);
};

} // namespace VulkanEngine::Components
