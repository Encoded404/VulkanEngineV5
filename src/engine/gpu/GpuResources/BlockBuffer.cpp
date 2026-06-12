module;

module VulkanEngine.GpuResources.BlockArray;

import std;
import std.compat;

import vulkan_hpp;

import VulkanBackend.Runtime.VulkanBootstrap;
import VulkanEngine.GpuBuffer;

namespace VulkanEngine::GpuResources {

BlockArray::~BlockArray() {
    Shutdown();
}

BlockArray::BlockArray(BlockArray&&) noexcept = default;
BlockArray& BlockArray::operator=(BlockArray&&) noexcept = default;

bool BlockArray::Initialize(VulkanEngine::Runtime::IVulkanBootstrap& backend, const Config& cfg) {
    if (cfg.entry_size == 0) return false;
    if (cfg.entries_per_block == 0) return false;
    backend_ = &backend;
    cfg_ = cfg;
    return true;
}

void BlockArray::Shutdown() {
    blocks_.clear();
    mappings_.clear();
    backend_ = nullptr;
}

bool BlockArray::AddBlock() {
    if (!backend_) return false;

    const uint64_t block_bytes = BlockSize();
    if (block_bytes == 0) return false;

    GpuBuffer buffer = GpuBuffer::Create(*backend_, block_bytes,
        vk::BufferUsageFlagBits::eStorageBuffer |
        vk::BufferUsageFlagBits::eTransferDst |
        cfg_.extra_usage,
        cfg_.memory);

    if (!buffer.IsValid()) return false;

    void* mapping = nullptr; // NOLINT
    if (cfg_.memory & vk::MemoryPropertyFlagBits::eHostVisible) {
        mapping = buffer.Map(0, block_bytes);
    }

    blocks_.push_back(std::move(buffer));
    mappings_.push_back(mapping);
    return true;
}

void* BlockArray::EnsureCapacity(uint32_t count) {
    if (!backend_) return nullptr;

    const uint32_t needed_blocks = (count + cfg_.entries_per_block - 1) / cfg_.entries_per_block;
    while (blocks_.size() < needed_blocks) {
        if (!AddBlock()) return nullptr;
    }

    if (mappings_.empty()) return nullptr;
    return mappings_[0];
}

void* BlockArray::Get(uint32_t index) {
    const uint32_t block_idx = index / cfg_.entries_per_block;
    const uint32_t local_idx = index % cfg_.entries_per_block;

    if (block_idx >= blocks_.size()) return nullptr;
    if (!mappings_[block_idx]) return nullptr;

    return static_cast<char*>(mappings_[block_idx]) + static_cast<uint64_t>(local_idx) * cfg_.entry_size;
}

vk::Buffer BlockArray::GetBlockArray(uint32_t block_index) const {
    if (block_index >= blocks_.size()) return nullptr;
    return *blocks_[block_index].GetBuffer();
}

} // namespace VulkanEngine::GpuResources
