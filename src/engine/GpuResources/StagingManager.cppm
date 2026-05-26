module;

#include <cstdint>
#include <memory>
#include <vector>

#include <vulkan/vulkan_raii.hpp>

export module VulkanEngine.StagingManager;

export import VulkanBackend.Runtime.VulkanBootstrap;
export import VulkanEngine.GpuBuffer;

export namespace VulkanEngine::GpuResources {

struct StagingSlice {
    uint32_t slot_index;
    uint64_t offset;
    void* data;
    vk::Buffer buffer;
    uint64_t size;
};

class StagingManager {
public:
    StagingManager() = default;
    ~StagingManager();

    StagingManager(const StagingManager&) = delete;
    StagingManager& operator=(const StagingManager&) = delete;
    StagingManager(StagingManager&&) = delete;
    StagingManager& operator=(StagingManager&&) = delete;

    bool Initialize(VulkanEngine::Runtime::IVulkanBootstrapBackend& backend,
                    uint64_t slot_size = 64ULL << 20, // 64 MiB
                    uint32_t slot_count = 3);
    void Shutdown();

    StagingSlice Allocate(uint64_t size, uint64_t alignment = 256);

    void RecordBufferCopy(const StagingSlice& slice,
                          vk::Buffer dst_buffer, uint64_t dst_offset);

    void RecordBufferToImage(const StagingSlice& slice,
                             vk::Image dst_image, uint32_t width, uint32_t height);

    void Flush();
    void WaitForSlot(uint32_t slot_index);
    void WaitForAll();

    [[nodiscard]] bool IsValid() const { return !slots_.empty(); }
    VulkanEngine::Runtime::IVulkanBootstrapBackend* GetBackend() { return backend_; }

private:
    struct Slot {
        VulkanEngine::GpuResources::GpuBuffer buffer;
        void* mapping = nullptr;
        vk::raii::CommandBuffer cmd_buffer = vk::raii::CommandBuffer(nullptr);
        vk::raii::Fence fence = vk::raii::Fence(nullptr);
        uint64_t bump_offset = 0;
        bool busy = false;
    };

    Slot& AcquireSlot();

    VulkanEngine::Runtime::IVulkanBootstrapBackend* backend_ = nullptr;
    std::unique_ptr<vk::raii::CommandPool> cmd_pool_;
    std::vector<Slot> slots_;
    uint64_t slot_size_ = 0;
    uint32_t current_slot_ = 0;
};

}
