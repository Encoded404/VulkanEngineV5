module;

#include <cstdint>
#include <vector>

#include <vulkan/vulkan_raii.hpp>

export module VulkanEngine.GpuResources.BlockBuffer;

import VulkanBackend.Runtime.VulkanBootstrap;
import VulkanEngine.GpuBuffer;

export namespace VulkanEngine::GpuResources {

class BlockBuffer {
public:
    struct Config {
        uint32_t entry_size = 0;
        uint32_t entries_per_block = 256;
        vk::BufferUsageFlags extra_usage = {};
        vk::MemoryPropertyFlags memory = vk::MemoryPropertyFlagBits::eHostVisible |
                                          vk::MemoryPropertyFlagBits::eHostCoherent;
    };

    BlockBuffer() = default;
    ~BlockBuffer();

    BlockBuffer(const BlockBuffer&) = delete;
    BlockBuffer& operator=(const BlockBuffer&) = delete;

    BlockBuffer(BlockBuffer&&) noexcept;
    BlockBuffer& operator=(BlockBuffer&&) noexcept;

    bool Initialize(VulkanEngine::Runtime::IVulkanBootstrapBackend& backend, const Config& cfg);

    void Shutdown();

    void* EnsureCapacity(uint32_t count);

    void* Get(uint32_t index);

    [[nodiscard]] uint32_t BlockCount() const { return static_cast<uint32_t>(blocks_.size()); }
    [[nodiscard]] vk::Buffer GetBlockBuffer(uint32_t block_index) const;
    [[nodiscard]] uint64_t BlockSize() const { return static_cast<uint64_t>(cfg_.entries_per_block) * cfg_.entry_size; }
    [[nodiscard]] uint32_t EntriesPerBlock() const { return cfg_.entries_per_block; }
    [[nodiscard]] uint32_t EntrySize() const { return cfg_.entry_size; }
    [[nodiscard]] bool IsValid() const { return backend_ != nullptr; }

private:
    bool AddBlock();

    VulkanEngine::Runtime::IVulkanBootstrapBackend* backend_ = nullptr;
    Config cfg_{};
    std::vector<GpuBuffer> blocks_;
    std::vector<void*> mappings_;
};

} // namespace VulkanEngine::GpuResources
