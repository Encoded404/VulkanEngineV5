module;

#include <cstdint>
#include <vector>

#include <vulkan/vulkan_raii.hpp>

export module VulkanEngine.GpuResources.HostRingPool;

export import VulkanBackend.Runtime.VulkanBootstrap;
export import VulkanEngine.GpuBuffer;

export namespace VulkanEngine::GpuResources {

class HostRingPool {
public:
    struct Config {
        uint64_t vertex_buffer_size = 64ULL << 20;
        uint64_t index_buffer_size = 32ULL << 20;
        uint32_t frames_in_flight = 3;
    };

    struct StreamedMeshHandle {
        // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
        uint32_t id = UINT32_MAX;
        uint64_t vertex_offset = 0;
        uint64_t vertex_max_size = 0;
        uint64_t index_offset = 0;
        uint64_t index_max_size = 0;
        // NOLINTEND(misc-non-private-member-variables-in-classes)

        [[nodiscard]] bool IsValid() const { return id != UINT32_MAX; }
    };

    HostRingPool() = default;
    ~HostRingPool();

    HostRingPool(const HostRingPool&) = delete;
    HostRingPool& operator=(const HostRingPool&) = delete;
    HostRingPool(HostRingPool&&) = delete;
    HostRingPool& operator=(HostRingPool&&) = delete;

    bool Initialize(VulkanEngine::Runtime::IVulkanBootstrap& backend,
                    const Config& config);
    void Shutdown();

    // Reserve space for a streamed mesh. Returns invalid handle on failure.
    StreamedMeshHandle RegisterStreamedMesh(uint64_t max_vertex_bytes,
                                            uint64_t max_index_bytes);

    // Get write pointers for the given frame's ring slot.
    [[nodiscard]] void* MapVertexData(StreamedMeshHandle handle, uint32_t frame_index) const;
    [[nodiscard]] void* MapIndexData(StreamedMeshHandle handle, uint32_t frame_index) const;

    // Buffers for descriptor array updates.
    [[nodiscard]] vk::Buffer GetVertexBuffer(uint32_t frame_index) const;
    [[nodiscard]] vk::Buffer GetIndexBuffer(uint32_t frame_index) const;
    [[nodiscard]] uint64_t GetVertexBufferSize() const { return config_.vertex_buffer_size; }
    [[nodiscard]] uint64_t GetIndexBufferSize() const { return config_.index_buffer_size; }
    [[nodiscard]] uint32_t GetFramesInFlight() const { return config_.frames_in_flight; }
    [[nodiscard]] bool IsValid() const { return !frames_.empty(); }

private:
    struct FrameData {
        GpuBuffer vertex_buffer;
        void* vertex_mapping = nullptr;
        GpuBuffer index_buffer;
        void* index_mapping = nullptr;
    };

    Config config_{};
    std::vector<FrameData> frames_;
    std::vector<StreamedMeshHandle> handles_;
    uint64_t next_vertex_offset_ = 0;
    uint64_t next_index_offset_ = 0;
};

} // namespace VulkanEngine::GpuResources
