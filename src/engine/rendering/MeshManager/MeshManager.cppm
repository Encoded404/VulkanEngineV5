module;

#include <cstdint>
#include <vector>

export module VulkanEngine.MeshManager;

export import VulkanEngine.GpuResources.DeviceBufferHeap;
export import VulkanEngine.GpuResources.StagingManager;
export import VulkanEngine.GpuResources.HostRingPool;
export import VulkanEngine.GpuResources.MeshData;
export import VulkanEngine.StandardMeshPipeline;
export import VulkanEngine.Mesh.MeshTypes;

export namespace VulkanEngine {

class MeshManager {
public:
    static constexpr uint32_t INVALID_HANDLE = UINT32_MAX;

    enum class Strategy : uint8_t { Persistent, Streamed };

    struct GpuMeshInfo {
        VulkanEngine::GpuResources::HeapAllocation vertex_allocation{};
        VulkanEngine::GpuResources::HeapAllocation index_allocation{};
        uint32_t vertex_buffer_index = 0;
        uint32_t index_buffer_index = 0;
        std::vector<SubMesh> sub_meshes;
    };

    struct Handle {
        // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
        uint32_t id = INVALID_HANDLE;
        // NOLINTEND(misc-non-private-member-variables-in-classes)
        [[nodiscard]] bool IsValid() const { return id != INVALID_HANDLE; }
    };

    MeshManager() = default;
    ~MeshManager();

    MeshManager(const MeshManager&) = delete;
    MeshManager& operator=(const MeshManager&) = delete;
    MeshManager(MeshManager&&) = delete;
    MeshManager& operator=(MeshManager&&) = delete;

    bool Initialize(VulkanEngine::Runtime::IVulkanBootstrap& backend,
                    VulkanEngine::GpuResources::DeviceBufferHeap* vertex_heap,
                    VulkanEngine::GpuResources::DeviceBufferHeap* index_heap,
                    VulkanEngine::GpuResources::StagingManager* staging_mgr,
                    VulkanEngine::GpuResources::HostRingPool* ring_pool);
    void Shutdown();

    // Persistent upload via staging to a device-local heap.
    // Blocks until the transfer completes on the GPU.
    Handle UploadPersistent(const VulkanEngine::GpuResources::MeshData& data);

    // Register a mesh for per-frame streaming via host-visible ring buffers.
    // The initial data is written to all ring slots to avoid uninitialized reads.
    Handle RegisterStreamed(const VulkanEngine::GpuResources::MeshData& initial_data);

    // Update a streamed mesh's data for the given frame index.
    // Copies vertex and index data into the current ring slot.
    void UpdateStreamed(Handle handle, const VulkanEngine::GpuResources::MeshData& data,
                        uint32_t frame_index);

    // Schedule a mesh for deferred deletion. The GPU resources are freed
    // after FRAMES_IN_FLIGHT frames have passed since the last EndFrame.
    void Remove(Handle handle);

    // Advance frame index, drain the deferred-free queue.
    void EndFrame(uint32_t frame_index);

    // Query GPU info for descriptor updates.
    [[nodiscard]] const GpuMeshInfo* GetMeshInfo(Handle handle) const;

    // Access the streamed mesh ring buffers for descriptor updates.
    [[nodiscard]] vk::Buffer GetStreamedVertexBuffer(uint32_t frame_index) const;
    [[nodiscard]] vk::Buffer GetStreamedIndexBuffer(uint32_t frame_index) const;
    [[nodiscard]] uint64_t GetStreamedVertexBufferSize() const;
    [[nodiscard]] uint64_t GetStreamedIndexBufferSize() const;

    [[nodiscard]] bool IsValid() const { return backend_ != nullptr; }

private:
    struct MeshEntry {
        Strategy strategy = Strategy::Persistent;
        GpuMeshInfo info{};
        // For streamed meshes only: fixed-offset handle in the ring pool
        VulkanEngine::GpuResources::HostRingPool::StreamedMeshHandle stream_handle{};
    };

    struct DeferredFree {
        VulkanEngine::GpuResources::HeapAllocation allocation;
        VulkanEngine::GpuResources::DeviceBufferHeap* heap = nullptr;
        int32_t frames_remaining = 0;
    };

    VulkanEngine::Runtime::IVulkanBootstrap* backend_ = nullptr;
    VulkanEngine::GpuResources::DeviceBufferHeap* vertex_heap_ = nullptr;
    VulkanEngine::GpuResources::DeviceBufferHeap* index_heap_ = nullptr;
    VulkanEngine::GpuResources::StagingManager* staging_mgr_ = nullptr;
    VulkanEngine::GpuResources::HostRingPool* ring_pool_ = nullptr;

    std::vector<MeshEntry> entries_;
    std::vector<uint32_t> free_handles_;
    std::vector<DeferredFree> deferred_free_queue_;
    uint32_t frames_in_flight_ = 3;
};

} // namespace VulkanEngine
