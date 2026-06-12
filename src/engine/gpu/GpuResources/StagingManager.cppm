module;

export module VulkanEngine.GpuResources.StagingManager;

import std;
import std.compat;

import vulkan_hpp;

export import VulkanBackend.Runtime.VulkanBootstrap;
export import VulkanEngine.GpuBuffer;

export namespace VulkanEngine::GpuResources {

struct StagingSlice {
    std::uint32_t slot_index;
    std::uint64_t offset;
    void* data;
    vk::Buffer buffer;
    std::uint64_t size;
};

class StagingManager {
public:
    StagingManager() = default;
    ~StagingManager();

    StagingManager(const StagingManager&) = delete;
    StagingManager& operator=(const StagingManager&) = delete;
    StagingManager(StagingManager&&) = delete;
    StagingManager& operator=(StagingManager&&) = delete;

    bool Initialize(VulkanEngine::Runtime::IVulkanBootstrap& backend,
                    std::uint64_t slot_size = 64ULL << 20, // 64 MiB
                    std::uint32_t slot_count = 3);
    void Shutdown();

    StagingSlice Allocate(std::uint64_t size, std::uint64_t alignment = 256);

    void RecordBufferCopy(const StagingSlice& slice,
                          vk::Buffer dst_buffer, std::uint64_t dst_offset);

    void RecordBufferToImage(const StagingSlice& slice,
                             vk::Image dst_image, std::uint32_t width, std::uint32_t height);

    void Flush();
    void WaitForSlot(std::uint32_t slot_index);
    void WaitForAll();

    [[nodiscard]] bool IsValid() const { return !slots_.empty(); }
    VulkanEngine::Runtime::IVulkanBootstrap* GetBackend() { return backend_; }

private:
    struct Slot {
        VulkanEngine::GpuResources::GpuBuffer buffer;
        void* mapping = nullptr;
        vk::raii::CommandBuffer cmd_buffer = vk::raii::CommandBuffer(nullptr);
        vk::raii::Fence fence = vk::raii::Fence(nullptr);
        std::uint64_t bump_offset = 0;
        bool busy = false;
    };

    Slot& AcquireSlot();

    VulkanEngine::Runtime::IVulkanBootstrap* backend_ = nullptr;
    std::unique_ptr<vk::raii::CommandPool> cmd_pool_;
    std::vector<Slot> slots_;
    std::uint64_t slot_size_ = 0;
    std::uint32_t current_slot_ = 0;
};

}
