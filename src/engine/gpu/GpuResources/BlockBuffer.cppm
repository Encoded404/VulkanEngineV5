module;

export module VulkanEngine.GpuResources.BlockArray;

import std;
import std.compat;

import vulkan_hpp;

import VulkanBackend.Runtime.VulkanBootstrap;
import VulkanEngine.GpuBuffer;

export namespace VulkanEngine::GpuResources {

class BlockArray {
public:
    struct Config {
        std::uint32_t entry_size = 0;
        std::uint32_t entries_per_block = 256;
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

    void* EnsureCapacity(std::uint32_t count);

    void* Get(std::uint32_t index);

    [[nodiscard]] std::uint32_t BlockCount() const { return static_cast<std::uint32_t>(blocks_.size()); }
    [[nodiscard]] vk::Buffer GetBlockArray(std::uint32_t block_index) const;
    [[nodiscard]] std::uint64_t BlockSize() const { return static_cast<std::uint64_t>(cfg_.entries_per_block) * cfg_.entry_size; }
    [[nodiscard]] std::uint32_t EntriesPerBlock() const { return cfg_.entries_per_block; }
    [[nodiscard]] std::uint32_t EntrySize() const { return cfg_.entry_size; }
    [[nodiscard]] bool IsValid() const { return backend_ != nullptr; }

private:
    bool AddBlock();

    VulkanEngine::Runtime::IVulkanBootstrap* backend_ = nullptr;
    Config cfg_{};
    std::vector<GpuBuffer> blocks_;
    std::vector<void*> mappings_;
};

} // namespace VulkanEngine::GpuResources
