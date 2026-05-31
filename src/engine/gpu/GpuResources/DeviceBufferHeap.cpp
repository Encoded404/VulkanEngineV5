module;

#include <cstdint>
#include <vector>

#include <vulkan/vulkan_raii.hpp>

#include <logging/logging.hpp>

module VulkanEngine.GpuResources.DeviceBufferHeap;

import VulkanBackend.Runtime.VulkanBootstrap;
import VulkanEngine.GpuBuffer;

namespace VulkanEngine::GpuResources {

DeviceBufferHeap::~DeviceBufferHeap() {
    Shutdown();
}

bool DeviceBufferHeap::Initialize(VulkanEngine::Runtime::IVulkanBootstrap& backend,
                                   const HeapConfig& config,
                                   const std::string& debug_name) {
    backend_ = &backend;
    config_ = config;
    debug_name_ = debug_name;
    if (config_.default_alignment == 0ULL) {
        config_.default_alignment = 256ULL;
    }

    LOGIFACE_LOG(debug, "DeviceBufferHeap '" + debug_name_ + "' initialized: block_size=" +
                 std::to_string(config_.block_size / (1024ULL * 1024ULL)) + " MB");
    return true;
}

void DeviceBufferHeap::Shutdown() {
    blocks_.clear();
    backend_ = nullptr;
}

uint32_t DeviceBufferHeap::AddBlock() {
    if (!backend_) return UINT32_MAX;

    const uint32_t index = static_cast<uint32_t>(blocks_.size());

    const vk::BufferUsageFlags usage = vk::BufferUsageFlagBits::eStorageBuffer |
                                       vk::BufferUsageFlagBits::eTransferDst |
                                       vk::BufferUsageFlagBits::eTransferSrc |
                                       config_.extra_usage;

    GpuBuffer buffer = GpuBuffer::Create(
        *backend_, config_.block_size, usage,
        vk::MemoryPropertyFlagBits::eDeviceLocal);

    if (!buffer.IsValid()) {
        LOGIFACE_LOG(error, "DeviceBufferHeap: failed to allocate block " + std::to_string(index));
        return UINT32_MAX;
    }

    Block block;
    block.buffer = std::move(buffer);
    block.size = config_.block_size;
    block.allocator.Initialize(config_.block_size);

    blocks_.push_back(std::move(block));

    LOGIFACE_LOG(debug, "DeviceBufferHeap '" + debug_name_ + "': added block " + std::to_string(index) +
                 " (" + std::to_string(config_.block_size / (1024ULL * 1024ULL)) + " MB)");
    return index;
}

HeapAllocation DeviceBufferHeap::Allocate(uint64_t size, uint64_t alignment) {
    if (size == 0) return {};
    if (alignment == 0) alignment = config_.default_alignment;

    for (uint32_t bi = 0; bi < static_cast<uint32_t>(blocks_.size()); ++bi) {
        auto& block = blocks_[bi];
        const uint64_t offset = block.allocator.Allocate(size, alignment);
        if (offset != UINT64_MAX) {
            HeapAllocation alloc{};
            alloc.buffer_index = bi;
            alloc.offset = offset;
            alloc.size = size;
            return alloc;
        }
    }

    // No space found — add a new block
    const uint32_t new_bi = AddBlock();
    if (new_bi == UINT32_MAX) return {};

    auto& new_block = blocks_[new_bi];
    const uint64_t offset = new_block.allocator.Allocate(size, alignment);
    if (offset == UINT64_MAX) return {};

    HeapAllocation alloc{};
    alloc.buffer_index = new_bi;
    alloc.offset = offset;
    alloc.size = size;
    return alloc;
}

void DeviceBufferHeap::Free(HeapAllocation& alloc) {
    if (alloc.buffer_index >= blocks_.size()) return;

    auto& block = blocks_[alloc.buffer_index];
    block.allocator.Free(alloc.offset, alloc.size);

    alloc = {};
}

vk::Buffer DeviceBufferHeap::GetBuffer(uint32_t index) const {
    if (index >= blocks_.size()) return VK_NULL_HANDLE;
    return *blocks_[index].buffer.GetBuffer();
}

} // namespace VulkanEngine::GpuResources
