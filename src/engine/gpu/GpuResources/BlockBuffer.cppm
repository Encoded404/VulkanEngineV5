module;

#include <cstdint>
#include <vector>

#include <vulkan/vulkan_raii.hpp>

export module VulkanEngine.GpuResources.BlockArray;

import VulkanBackend.Runtime.VulkanBootstrap;
import VulkanEngine.GpuBuffer;

export namespace VulkanEngine::GpuResources {

class BlockArray {
public:
    struct Config {
        uint32_t entry_size = 0;
        uint32_t entries_per_block = 256;
        vk::BufferUsageFlags extra_usage = {};
        vk::MemoryPropertyFlags memory = vk::MemoryPropertyFlagBits::eHostVisible |
                                          vk::MemoryPropertyFlagBits::eHostCoherent;
    };

    BlockArray() = default;
    ~BlockArray();

    BlockArray(const BlockArray&) = delete;
    BlockArray& operator=(const BlockArray&) = delete;

    BlockArray(BlockArray&&) noexcept;
    BlockArray& operator=(BlockArray&&) noexcept;

    bool Initialize(VulkanEngine::Runtime::IVulkanBootstrap& backend, const Config& cfg);

    void Shutdown();

    void* EnsureCapacity(uint32_t count);

    void* Get(uint32_t index);

    [[nodiscard]] uint32_t BlockCount() const { return static_cast<uint32_t>(blocks_.size()); }
    [[nodiscard]] vk::Buffer GetBlockArray(uint32_t block_index) const;
    [[nodiscard]] uint64_t BlockSize() const { return static_cast<uint64_t>(cfg_.entries_per_block) * cfg_.entry_size; }
    [[nodiscard]] uint32_t EntriesPerBlock() const { return cfg_.entries_per_block; }
    [[nodiscard]] uint32_t EntrySize() const { return cfg_.entry_size; }
    [[nodiscard]] bool IsValid() const { return backend_ != nullptr; }

private:
    bool AddBlock();

    VulkanEngine::Runtime::IVulkanBootstrap* backend_ = nullptr;
    Config cfg_{};
    std::vector<GpuBuffer> blocks_;
    std::vector<void*> mappings_;
};

} // namespace VulkanEngine::GpuResources
