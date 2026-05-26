module;

#include <cstdint>
#include <vector>

#include <vulkan/vulkan_raii.hpp>

#include <logging/logging.hpp>

module VulkanEngine.DeviceBufferHeap;

import VulkanBackend.Runtime.VulkanBootstrap;
import VulkanEngine.GpuBuffer;

namespace VulkanEngine::GpuResources {

DeviceBufferHeap::~DeviceBufferHeap() {
    Shutdown();
}

bool DeviceBufferHeap::Initialize(VulkanEngine::Runtime::IVulkanBootstrapBackend& backend,
                                   const HeapConfig& config,
                                   const std::string& debug_name) {
    backend_ = &backend;
    config_ = config;
    debug_name_ = debug_name;
    if (config_.default_alignment == 0ULL) {
        config_.default_alignment = 256ULL;
    }

    LOGIFACE_LOG(info, "DeviceBufferHeap '" + debug_name_ + "' initialized: block_size=" +
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
    block.free_list.push_back({0, config_.block_size});

    blocks_.push_back(std::move(block));

    LOGIFACE_LOG(info, "DeviceBufferHeap '" + debug_name_ + "': added block " + std::to_string(index) +
                 " (" + std::to_string(config_.block_size / (1024ULL * 1024ULL)) + " MB)");
    return index;
}

HeapAllocation DeviceBufferHeap::Allocate(uint64_t size, uint64_t alignment) {
    if (size == 0) return {};
    if (alignment == 0) alignment = config_.default_alignment;

    // Try each existing block
    for (uint32_t bi = 0; bi < static_cast<uint32_t>(blocks_.size()); ++bi) {
        auto& block = blocks_[bi];
        for (auto fi = block.free_list.begin(); fi != block.free_list.end(); ++fi) {
            uint64_t aligned = fi->offset;
            const uint64_t mod = fi->offset % alignment;
            if (mod != 0) aligned += alignment - mod;

            if (aligned + size > fi->offset + fi->size) continue;

            HeapAllocation alloc{};
            alloc.buffer_index = bi;
            alloc.offset = aligned;
            alloc.size = size;

            const uint64_t free_end = fi->offset + fi->size;
            const uint64_t alloc_end = aligned + size;

            if (aligned == fi->offset && size == fi->size) {
                block.free_list.erase(fi);
            } else if (aligned == fi->offset) {
                fi->offset = alloc_end;
                fi->size = free_end - alloc_end;
            } else if (alloc_end == free_end) {
                fi->size = aligned - fi->offset;
            } else {
                // Hole in the middle: split
                const uint64_t before_size = aligned - fi->offset;
                fi->offset = alloc_end;
                fi->size = free_end - alloc_end;
                block.free_list.insert(fi, {aligned - before_size, before_size});
            }

            return alloc;
        }
    }

    // No space found — add a new block
    const uint32_t new_bi = AddBlock();
    if (new_bi == UINT32_MAX) return {};

    auto& new_block = blocks_[new_bi];

    constexpr uint64_t aligned = 0ULL;
    HeapAllocation alloc{};
    alloc.buffer_index = new_bi;
    alloc.offset = aligned;
    alloc.size = size;

    if (size == new_block.size) {
        new_block.free_list.clear();
    } else {
        new_block.free_list[0].offset = size;
        new_block.free_list[0].size = new_block.size - size;
    }

    return alloc;
}

void DeviceBufferHeap::Free(HeapAllocation& alloc) {
    if (alloc.buffer_index >= blocks_.size()) return;

    auto& block = blocks_[alloc.buffer_index];
    auto& fl = block.free_list;

    // Insert sorted by offset, merging adjacent
    FreeBlock new_free{alloc.offset, alloc.size};
    bool merged = false;

    for (auto fi = fl.begin(); fi != fl.end(); ++fi) {
        // Check if adjacent before
        if (fi->offset + fi->size == new_free.offset) {
            fi->size += new_free.size;
            new_free = *fi;
            fl.erase(fi);
            merged = true;
            break;
        }
        // Check if adjacent after
        if (new_free.offset + new_free.size == fi->offset) {
            new_free.size += fi->size;
            fl.erase(fi);
            merged = true;
            break;
        }
    }

    if (!merged) {
        fl.push_back(new_free);
    } else {
        // Try merging again with remaining neighbors (one more pass)
        for (auto &[offset, size] : fl) {
            if (offset + size == new_free.offset) {
                size += new_free.size;
                alloc = {};
                return;
            }
            if (new_free.offset + new_free.size == offset) {
                offset = new_free.offset;
                size += new_free.size;
                alloc = {};
                return;
            }
        }
        fl.push_back(new_free);
    }

    alloc = {};
}

vk::Buffer DeviceBufferHeap::GetBuffer(uint32_t index) const {
    if (index >= blocks_.size()) return VK_NULL_HANDLE;
    return *blocks_[index].buffer.GetBuffer();
}

} // namespace VulkanEngine::GpuResources
