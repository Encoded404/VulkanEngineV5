module;

#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan_raii.hpp>

export module VulkanEngine.GpuResources.DeviceBufferHeap;

export import VulkanBackend.Runtime.VulkanBootstrap;
export import VulkanEngine.GpuBuffer;

export namespace VulkanEngine::GpuResources {

struct HeapConfig {
    uint64_t block_size = 128ULL << 20; // 128 MiB
    vk::BufferUsageFlags extra_usage = {};
    uint64_t default_alignment = 256ULL; // this is the minimum byte alignment for sub-allocations on the heap, older gpu's usually have 256 bytes, while newer have 16-64
};

struct HeapAllocation {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    uint32_t buffer_index = UINT32_MAX;
    uint64_t offset = 0;
    uint64_t size = 0;
    // NOLINTEND(misc-non-private-member-variables-in-classes)

    [[nodiscard]] bool IsValid() const { return buffer_index != UINT32_MAX; }
};

class DeviceBufferHeap {
public:
    DeviceBufferHeap() = default;
    ~DeviceBufferHeap();

    DeviceBufferHeap(const DeviceBufferHeap&) = delete;
    DeviceBufferHeap& operator=(const DeviceBufferHeap&) = delete;
    DeviceBufferHeap(DeviceBufferHeap&&) = delete;
    DeviceBufferHeap& operator=(DeviceBufferHeap&&) = delete;

    bool Initialize(VulkanEngine::Runtime::IVulkanBootstrap& backend,
                    const HeapConfig& config = {},
                    const std::string& debug_name = "unnamed");
    void Shutdown();

    HeapAllocation Allocate(uint64_t size, uint64_t alignment = 0);
    void Free(HeapAllocation& alloc);

    [[nodiscard]] vk::Buffer GetBuffer(uint32_t index) const;
    [[nodiscard]] uint32_t GetBufferCount() const { return static_cast<uint32_t>(blocks_.size()); }
    [[nodiscard]] const HeapConfig& GetConfig() const { return config_; }
    [[nodiscard]] const std::string& GetDebugName() const { return debug_name_; }
    VulkanEngine::Runtime::IVulkanBootstrap* GetBackend() { return backend_; }
    [[nodiscard]] bool IsValid() const { return backend_ != nullptr; }

private:
    struct FreeBlock {
        uint64_t offset;
        uint64_t size;
    };

    struct Block {
        VulkanEngine::GpuResources::GpuBuffer buffer;
        uint64_t size;
        std::vector<FreeBlock> free_list;
    };

    uint32_t AddBlock();

    VulkanEngine::Runtime::IVulkanBootstrap* backend_ = nullptr;
    std::vector<Block> blocks_;
    HeapConfig config_;
    std::string debug_name_ = "unnamed";
};

}
