module;

#include <cstdint>

export module VulkanEngine.Components.DynamicMesh;

import VulkanBackend.Component;
import VulkanEngine.GpuResources.MeshData;
import VulkanEngine.MeshManager;

export namespace VulkanEngine::Components {

class DynamicMesh : public VulkanEngine::Component {
public:
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    MeshManager::Handle gpu_handle;
    GpuResources::MeshData mesh_data;
    uint32_t first_submesh = 0;
    uint32_t submesh_count = 0;
    // NOLINTEND(misc-non-private-member-variables-in-classes)

    void SetupStreamed(MeshManager& mgr, const GpuResources::MeshData& initial);
};

}

