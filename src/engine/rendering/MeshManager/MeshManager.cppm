module;

export module VulkanEngine.MeshManager;

export import VulkanEngine.GpuResources.DeviceBufferHeap;
export import VulkanEngine.GpuResources.StagingManager;
export import VulkanEngine.GpuResources.MeshData;
export import VulkanEngine.StandardMeshPipeline;
export import VulkanEngine.Mesh.MeshTypes;

import std;

import vulkan_hpp;

constexpr std::uint32_t UINT32_MAX =
    std::numeric_limits<std::uint32_t>::max();

export namespace VulkanEngine {

class MeshManager {
public:
    static constexpr std::uint32_t INVALID_HANDLE = UINT32_MAX;
    static constexpr std::uint32_t MAX_FRAMES_IN_FLIGHT = 3;

    enum class Strategy : std::uint8_t { Persistent, Streamed };

    struct GpuMeshInfo {
        VulkanEngine::GpuResources::HeapAllocation vertex_allocation{};
        VulkanEngine::GpuResources::HeapAllocation index_allocation{};
        std::uint32_t vertex_buffer_index = 0;
        std::uint32_t index_buffer_index = 0;
        std::array<VulkanEngine::GpuResources::HeapAllocation, MAX_FRAMES_IN_FLIGHT> streamed_vertex_alloc{};
        std::array<VulkanEngine::GpuResources::HeapAllocation, MAX_FRAMES_IN_FLIGHT> streamed_index_alloc{};
        std::vector<SubMesh> sub_meshes;
    };

    struct Handle {
        // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
        std::uint32_t id = INVALID_HANDLE;
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
                    std::uint32_t frames_in_flight);
    void Shutdown();

    Handle UploadPersistent(const VulkanEngine::GpuResources::MeshData& data);

    Handle RegisterStreamed(const VulkanEngine::GpuResources::MeshData& initial_data);

    void UpdateStreamed(Handle handle, const VulkanEngine::GpuResources::MeshData& data,
                        std::uint32_t frame_index);

    void Remove(Handle handle);

    void EndFrame(std::uint32_t frame_index);

    [[nodiscard]] const GpuMeshInfo* GetMeshInfo(Handle handle) const;

    [[nodiscard]] vk::Buffer GetDynamicVertexBuffer(std::uint32_t fif_index, std::uint32_t buffer_index) const;
    [[nodiscard]] vk::Buffer GetDynamicIndexBuffer(std::uint32_t fif_index, std::uint32_t buffer_index) const;
    [[nodiscard]] std::uint64_t GetDynamicVertexBlockSize(std::uint32_t fif_index) const;
    [[nodiscard]] std::uint64_t GetDynamicIndexBlockSize(std::uint32_t fif_index) const;
    [[nodiscard]] std::uint32_t GetDynamicVertexBlockCount(std::uint32_t fif_index) const;
    [[nodiscard]] std::uint32_t GetDynamicIndexBlockCount(std::uint32_t fif_index) const;

    [[nodiscard]] bool IsValid() const { return backend_ != nullptr; }

private:
    struct MeshEntry {
        Strategy strategy = Strategy::Persistent;
        GpuMeshInfo info{};
    };

    struct DeferredFree {
        VulkanEngine::GpuResources::HeapAllocation allocation;
        VulkanEngine::GpuResources::DeviceBufferHeap* heap = nullptr;
        std::int32_t frames_remaining = 0;
    };

    VulkanEngine::Runtime::IVulkanBootstrap* backend_ = nullptr;
    VulkanEngine::GpuResources::DeviceBufferHeap* vertex_heap_ = nullptr;
    VulkanEngine::GpuResources::DeviceBufferHeap* index_heap_ = nullptr;
    VulkanEngine::GpuResources::StagingManager* staging_mgr_ = nullptr;
    VulkanEngine::GpuResources::DeviceBufferHeap* dynamic_vertex_heaps_ = nullptr;
    VulkanEngine::GpuResources::DeviceBufferHeap* dynamic_index_heaps_ = nullptr;

    std::vector<MeshEntry> entries_;
    std::vector<std::uint32_t> free_handles_;
    std::vector<DeferredFree> deferred_free_queue_;
    std::uint32_t frames_in_flight_ = 3;
};

} // namespace VulkanEngine
