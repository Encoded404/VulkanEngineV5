module;

export module VulkanEngine.GpuResources.GpuImageHeap;

import std;
import std.compat;

import vulkan_hpp;

export import VulkanBackend.Vulkan.VulkanBootstrap;

import VulkanEngine.GpuResources.TlsfAllocator;

export namespace VulkanEngine::GpuResources {

struct ImageHeapConfig {
    std::uint64_t block_size = 64ULL << 20; // 64 MiB
    vk::MemoryPropertyFlags memory_flags = vk::MemoryPropertyFlagBits::eDeviceLocal;
};

struct HeapImage {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::uint32_t image_index = std::numeric_limits<std::uint32_t>::max();
    bool dedicated = false;
    // NOLINTEND(misc-non-private-member-variables-in-classes)

    [[nodiscard]] bool IsValid() const {
        return image_index != std::numeric_limits<std::uint32_t>::max();
    }
};

// Device memory heap that owns sub-allocated VkImages. Mirrors
// DeviceBufferHeap: block memory + TLSF, with images bound at sub-offsets.
// Dedicated allocations are used whenever the driver requires them, and
// sub-allocations are aligned to bufferImageGranularity so an image never
// straddles a linear/non-linear boundary with a neighbouring allocation.
class GpuImageHeap {
public:
    GpuImageHeap() = default;
    ~GpuImageHeap();

    GpuImageHeap(const GpuImageHeap&) = delete;
    GpuImageHeap& operator=(const GpuImageHeap&) = delete;
    GpuImageHeap(GpuImageHeap&&) noexcept = default;
    GpuImageHeap& operator=(GpuImageHeap&&) noexcept = default;

    bool Initialize(VulkanBackend::Vulkan::IVulkanBootstrap& backend,
                    const ImageHeapConfig& config = {},
                    const std::string& debug_name = "unnamed");
    void Shutdown();

    // Creates `image_info` (initial layout is forced Undefined) and binds it to
    // either a dedicated allocation or a sub-allocated block region.
    HeapImage Allocate(vk::ImageCreateInfo image_info, vk::ImageAspectFlags aspect = vk::ImageAspectFlagBits::eColor);
    void Free(HeapImage& image);

    [[nodiscard]] vk::Image GetImage(std::uint32_t image_index) const;
    [[nodiscard]] vk::ImageView GetImageView(std::uint32_t image_index) const;
    [[nodiscard]] bool IsDedicated(std::uint32_t image_index) const;
    [[nodiscard]] std::uint64_t GetBufferImageGranularity() const { return buffer_image_granularity_; }
    [[nodiscard]] bool IsValid() const { return backend_ != nullptr; }
    [[nodiscard]] const std::string& GetDebugName() const { return debug_name_; }

private:
    struct Block {
        std::unique_ptr<vk::raii::DeviceMemory> memory;
        std::uint64_t size = 0;
        std::uint32_t memory_type_index = 0;
        TlsfAllocator allocator;
    };

    struct ImageRecord {
        std::unique_ptr<vk::raii::Image> image;
        std::unique_ptr<vk::raii::ImageView> view;
        std::unique_ptr<vk::raii::DeviceMemory> dedicated_memory;
        std::uint32_t block_index = std::numeric_limits<std::uint32_t>::max();
        std::uint64_t offset = 0;
        std::uint64_t size = 0;
        std::uint64_t alignment = 1;
        bool dedicated = false;
    };

    // Creates a fresh device-memory block of the given type (at least min_size).
    [[nodiscard]] std::uint32_t CreateBlock(std::uint32_t memory_type_index, std::uint64_t min_size);

    VulkanBackend::Vulkan::IVulkanBootstrap* backend_ = nullptr;
    ImageHeapConfig config_{};
    std::string debug_name_ = "unnamed";
    std::uint64_t buffer_image_granularity_ = 1;

    std::vector<Block> blocks_{};
    std::vector<ImageRecord> images_{};
};

} // namespace VulkanEngine::GpuResources
