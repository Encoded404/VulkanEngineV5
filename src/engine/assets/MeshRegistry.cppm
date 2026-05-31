module;

#include <cstdint>
#include <vector>
#include <filesystem>
#include <unordered_set>

export module VulkanEngine.MeshRegistry;

export import VulkanEngine.GpuResources.MeshData;
export import VulkanEngine.MeshManager;
export import VulkanEngine.SceneRenderer;
export import VulkanEngine.GpuResources.DeviceBufferHeap;
export import VulkanEngine.Mesh.MeshTypes;

export namespace VulkanEngine {

struct LoadedMeshInfo {
    GpuResources::MeshData cpu_data;
    MeshManager::Handle gpu_handle;
    uint32_t first_submesh_in_renderer = 0;
    uint32_t submesh_count = 0;
    bool pending_upload = false;
    bool gpu_resident = false;
    uint32_t frames_since_last_reference = 0;
};

class MeshRegistry {
public:
    MeshRegistry() = default;
    ~MeshRegistry();

    MeshRegistry(const MeshRegistry&) = delete;
    MeshRegistry& operator=(const MeshRegistry&) = delete;

    // Register CPU-generated mesh data, return a mesh ID
    uint32_t Register(const GpuResources::MeshData& data);

    // Mark a mesh as needed this frame. If not GPU-resident, queue for upload.
    void RequestGpuResidency(uint32_t mesh_id, MeshManager& mgr,
                             SceneRenderer::SceneRenderer& renderer,
                             GpuResources::DeviceBufferHeap& vtx_heap,
                             GpuResources::DeviceBufferHeap& idx_heap);

    // Release a mesh reference. Decrements usage tracking.
    void Release(uint32_t mesh_id, MeshManager& mgr);

    // Called each frame. Decrements stale counters. Evicts unreferenced meshes.
    void EndFrame(MeshManager& mgr, uint32_t eviction_timeout_frames);

    [[nodiscard]] const LoadedMeshInfo* Get(uint32_t mesh_id) const;
    [[nodiscard]] bool IsValid() const { return true; }

    void Shutdown();

private:
    std::vector<LoadedMeshInfo> entries_;
    std::vector<uint32_t> free_ids_;
    std::unordered_set<uint32_t> referenced_this_frame_;
};

}

