module;

#include <cstdint>

module VulkanEngine.Components.DynamicMesh;

import VulkanEngine.MeshManager;
import VulkanEngine.GpuResources.MeshData;

namespace VulkanEngine::Components {

void DynamicMesh::SetupStreamed(MeshManager& mgr, const GpuResources::MeshData& initial) {
    gpu_handle = mgr.RegisterStreamed(initial);
    if (gpu_handle.IsValid()) {
        mesh_data = initial;
        first_submesh = 0;
        submesh_count = static_cast<uint32_t>(initial.sub_meshes.empty()
            ? 1 : initial.sub_meshes.size());
    }
}

}

