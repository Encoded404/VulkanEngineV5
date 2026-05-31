module;

#include <array>
#include <cstdint>
#include <vector>

export module VulkanEngine.MeshManager;

export import VulkanEngine.GpuResources.DeviceBufferHeap;
export import VulkanEngine.GpuResources.StagingManager;
export import VulkanEngine.GpuResources.MeshData;
export import VulkanEngine.StandardMeshPipeline;
export import VulkanEngine.Mesh.MeshTypes;

export namespace VulkanEngine {

class MeshManager {
public:
    static constexpr uint32_t INVALID_HANDLE = UINT32_MAX;
    static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 3;

    enum class Strategy : uint8_t { Persistent, Streamed };

    struct GpuMeshInfo {
        VulkanEngine::GpuResources::HeapAllocation vertex_allocation{};
        VulkanEngine::GpuResources::HeapAllocation index_allocation{};
        uint32_t vertex_buffer_index = 0;
        uint32_t index_buffer_index = 0;
        std::array<VulkanEngine::GpuResources::HeapAllocation, MAX_FRAMES_IN_FLIGHT> streamed_vertex_alloc{};
        std::array<VulkanEngine::GpuResources::HeapAllocation, MAX_FRAMES_IN_FLIGHT> streamed_index_alloc{};
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
                    VulkanEngine::GpuResources::DeviceBufferHeap* dynamic_vertex_heaps,
                    VulkanEngine::GpuResources::DeviceBufferHeap* dynamic_index_heaps,
                    uint32_t frames_in_flight);
    void Shutdown();

    Handle UploadPersistent(const VulkanEngine::GpuResources::MeshData& data);

    Handle RegisterStreamed(const VulkanEngine::GpuResources::MeshData& initial_data);

    void UpdateStreamed(Handle handle, const VulkanEngine::GpuResources::MeshData& data,
                        uint32_t frame_index);

    void Remove(Handle handle);

    void EndFrame(uint32_t frame_index);

    [[nodiscard]] const GpuMeshInfo* GetMeshInfo(Handle handle) const;

    [[nodiscard]] vk::Buffer GetDynamicVertexBuffer(uint32_t fif_index, uint32_t buffer_index) const;
    [[nodiscard]] vk::Buffer GetDynamicIndexBuffer(uint32_t fif_index, uint32_t buffer_index) const;
    [[nodiscard]] uint64_t GetDynamicVertexBlockSize(uint32_t fif_index) const;
    [[nodiscard]] uint64_t GetDynamicIndexBlockSize(uint32_t fif_index) const;
    [[nodiscard]] uint32_t GetDynamicVertexBlockCount(uint32_t fif_index) const;
    [[nodiscard]] uint32_t GetDynamicIndexBlockCount(uint32_t fif_index) const;

    [[nodiscard]] bool IsValid() const { return backend_ != nullptr; }

private:
    struct MeshEntry {
        Strategy strategy = Strategy::Persistent;
        GpuMeshInfo info{};
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
    VulkanEngine::GpuResources::DeviceBufferHeap* dynamic_vertex_heaps_ = nullptr;
    VulkanEngine::GpuResources::DeviceBufferHeap* dynamic_index_heaps_ = nullptr;

    std::vector<MeshEntry> entries_;
    std::vector<uint32_t> free_handles_;
    std::vector<DeferredFree> deferred_free_queue_;
    uint32_t frames_in_flight_ = 3;
};

} // namespace VulkanEngine
