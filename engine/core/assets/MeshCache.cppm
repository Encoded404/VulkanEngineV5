module;

export module VulkanEngine.MeshRegistry;

import std;

export import VulkanEngine.GpuResources.MeshData;
export import VulkanEngine.MeshManager;
export import VulkanEngine.SceneRenderer;
export import VulkanEngine.GpuResources.DeviceBufferHeap;
export import VulkanEngine.Mesh.MeshTypes;

export namespace VulkanEngine {

// Records which table elements a frame has already published.
// A publish writes one descriptor per in-flight frame, so the second mesh that lands in the same heap block only repeats that work.
class TablePublishRecord {
public:
    // Returns true when this table and block pair is new, so the caller writes it.
    bool Note(SceneGpuLayout::TableId table, std::uint32_t index);
    [[nodiscard]] bool Contains(SceneGpuLayout::TableId table, std::uint32_t index) const;
    void Clear();

private:
    std::array<std::unordered_set<std::uint32_t>, SceneGpuLayout::kTableCount> seen_{};
};

struct LoadedMeshInfo {
    GpuResources::MeshData cpu_data;
    MeshManager::Handle gpu_handle;
    std::uint32_t first_submesh_in_renderer = 0;
    std::uint32_t submesh_count = 0;
    bool pending_upload = false;
    bool gpu_resident = false;
    std::uint32_t frames_since_last_reference = 0;
};

class MeshRegistry {
public:
    MeshRegistry() = default;
    ~MeshRegistry();

    MeshRegistry(const MeshRegistry&) = delete;
    MeshRegistry& operator=(const MeshRegistry&) = delete;

    // Register CPU-generated mesh data, return a mesh ID
    std::uint32_t Register(const GpuResources::MeshData& data);

    // Mark a mesh as needed this frame. If not GPU-resident, queue for upload.
    void RequestGpuResidency(std::uint32_t mesh_id, MeshManager& mgr,
                             SceneRenderer::SceneRenderer& renderer,
                             GpuResources::DeviceBufferHeap& vtx_heap,
                             GpuResources::DeviceBufferHeap& idx_heap,
                             GpuResources::DeviceBufferHeap& uv_heap);

    // Release a mesh reference. Decrements usage tracking.
    void Release(std::uint32_t mesh_id, MeshManager& mgr);

    // Called each frame. Decrements stale counters. Evicts unreferenced meshes.
    void EndFrame(MeshManager& mgr, std::uint32_t eviction_timeout_frames);

    [[nodiscard]] const LoadedMeshInfo* Get(std::uint32_t mesh_id) const;
    [[nodiscard]] bool IsValid() const { return true; }

    // Whether this frame has already published this table element.
    // The residency tests read it: the descriptor write is a no-op without a device.
    [[nodiscard]] bool WasPublished(SceneGpuLayout::TableId table, std::uint32_t index) const {
        return published_this_frame_.Contains(table, index);
    }

    void Shutdown();

private:
    std::vector<LoadedMeshInfo> entries_;
    std::vector<std::uint32_t> free_ids_;
    std::unordered_set<std::uint32_t> referenced_this_frame_;
    TablePublishRecord published_this_frame_;
};

}

