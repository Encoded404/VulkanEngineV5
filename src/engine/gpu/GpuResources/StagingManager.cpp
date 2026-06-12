module;

#include <logging/logging.hpp>

module VulkanEngine.GpuResources.StagingManager;

import std;
import std.compat;

import vulkan_hpp;

import VulkanBackend.Runtime.VulkanBootstrap;
import VulkanBackend.Utils.VulkanDebugUtils;
import VulkanEngine.GpuBuffer;

constexpr std::uint32_t UINT64_T_MAX =
    std::numeric_limits<std::uint64_t>::max();

namespace VulkanEngine::GpuResources {

StagingManager::~StagingManager() {
    Shutdown();
}

bool StagingManager::Initialize(VulkanEngine::Runtime::IVulkanBootstrap& backend,
                                 uint64_t slot_size,
                                 uint32_t slot_count) {
    if (slot_count == 0 || slot_size == 0) return false;

    backend_ = &backend;
    slot_size_ = slot_size;
    current_slot_ = 0;

    const auto& device = backend.GetDevice();

    // Create command pool
    vk::CommandPoolCreateInfo pool_info{};
    pool_info.queueFamilyIndex = backend.GetGraphicsQueueFamily();
    pool_info.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
    cmd_pool_ = std::make_unique<vk::raii::CommandPool>(device, pool_info);
    VulkanEngine::Utils::SetVulkanObjectName(device, *cmd_pool_, "staging-command-pool");

    vk::CommandBufferAllocateInfo cmd_alloc{};
    cmd_alloc.commandPool = **cmd_pool_;
    cmd_alloc.level = vk::CommandBufferLevel::ePrimary;
    cmd_alloc.commandBufferCount = slot_count;

    auto cmd_bufs = device.allocateCommandBuffers(cmd_alloc);

    slots_.reserve(slot_count);
    for (uint32_t i = 0; i < slot_count; ++i) {
        Slot slot;
        slot.buffer = GpuBuffer::Create(
            backend, slot_size,
            vk::BufferUsageFlagBits::eTransferSrc,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

        if (!slot.buffer.IsValid()) {
            LOGIFACE_LOG(error, "StagingManager: failed to create staging buffer " + std::to_string(i));
            Shutdown();
            return false;
        }

        slot.mapping = slot.buffer.Map(0, slot_size);
        if (!slot.mapping) {
            LOGIFACE_LOG(error, "StagingManager: failed to map staging buffer " + std::to_string(i));
            Shutdown();
            return false;
        }

        slot.cmd_buffer = std::move(cmd_bufs[i]);

        vk::FenceCreateInfo fence_info{};
        fence_info.flags = vk::FenceCreateFlagBits::eSignaled;
        slot.fence = vk::raii::Fence(device, fence_info);
        VulkanEngine::Utils::SetVulkanObjectName(device, slot.fence, "staging-fence-" + std::to_string(i));

        slot.bump_offset = 0;
        slot.busy = false;

        slots_.push_back(std::move(slot));
    }

    LOGIFACE_LOG(debug, "StagingManager initialized: " + std::to_string(slot_count) +
                 " slots x " + std::to_string(slot_size / (1024ull * 1024)) + " MB");
    return true;
}

void StagingManager::Shutdown() {
    if (backend_) {
        try {
            WaitForAll();
        } catch (const std::exception& err) {
            LOGIFACE_LOG(error, "Error while waiting for staging manager shutdown: " + std::string(err.what()));
        }
    }

    for (auto& slot : slots_) {
        if (slot.mapping && slot.buffer.IsValid()) {
            slot.buffer.Unmap();
            slot.mapping = nullptr;
        }
        if (*slot.fence) {
            slot.fence = vk::raii::Fence(nullptr);
        }
        if (*slot.cmd_buffer) {
            slot.cmd_buffer = vk::raii::CommandBuffer(nullptr);
        }
        slot.buffer = GpuBuffer{};
    }
    slots_.clear();
    cmd_pool_.reset();
    backend_ = nullptr;
}

StagingManager::Slot& StagingManager::AcquireSlot() {
    for (uint32_t i = 0; i < static_cast<uint32_t>(slots_.size()); ++i) {
        const uint32_t idx = (current_slot_ + i) % static_cast<uint32_t>(slots_.size());
        auto& slot = slots_[idx];
        if (!slot.busy) {
            if (*slot.fence) {
                const auto result = backend_->GetDevice().waitForFences(*slot.fence, vk::True, UINT64_T_MAX);
                if (result != vk::Result::eSuccess) {
                    throw std::runtime_error("StagingManager: fence wait failed");
                }
                backend_->GetDevice().resetFences(*slot.fence);
            }
            slot.cmd_buffer.reset({});
            slot.cmd_buffer.begin({vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
            slot.bump_offset = 0;
            slot.busy = true;
            current_slot_ = (idx + 1) % static_cast<uint32_t>(slots_.size());
            return slot;
        }
    }

    // All slots busy — wait for the oldest (current_slot_)
    {
        auto& slot = slots_[current_slot_];
        if (*slot.fence) {
            const auto result = backend_->GetDevice().waitForFences(*slot.fence, vk::True, UINT64_T_MAX);
            if (result != vk::Result::eSuccess) {
                throw std::runtime_error("StagingManager: fence wait failed on full wait");
            }
            backend_->GetDevice().resetFences(*slot.fence);
        }
        slot.cmd_buffer.reset({});
        slot.cmd_buffer.begin({vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
        slot.bump_offset = 0;
        slot.busy = true;
        auto& ret = slots_[current_slot_];
        current_slot_ = (current_slot_ + 1) % static_cast<uint32_t>(slots_.size());
        return ret;
    }
}

StagingSlice StagingManager::Allocate(uint64_t size, uint64_t alignment) {
    if (slots_.empty() || size > slot_size_) {
        LOGIFACE_LOG(error, "StagingManager::Allocate: invalid size " + std::to_string(size));
        return {};
    }

    auto& slot = AcquireSlot();

    const uint64_t misalignment = slot.bump_offset % alignment;
    if (misalignment != 0) {
        slot.bump_offset += alignment - misalignment;
    }

    if (slot.bump_offset + size > slot_size_) {
        LOGIFACE_LOG(error, "StagingManager: staging buffer slot overflow");
        return {};
    }

    StagingSlice slice{};
    slice.slot_index = static_cast<uint32_t>(&slot - slots_.data());
    slice.offset = slot.bump_offset;
    slice.data = static_cast<uint8_t*>(slot.mapping) + slot.bump_offset;
    slice.buffer = *slot.buffer.GetBuffer();
    slice.size = size;

    slot.bump_offset += size;
    return slice;
}

void StagingManager::RecordBufferCopy(const StagingSlice& slice,
                                       vk::Buffer dst_buffer, uint64_t dst_offset) {
    if (slice.slot_index >= slots_.size()) return;
    auto& slot = slots_[slice.slot_index];
    if (!*slot.cmd_buffer) return;

    vk::BufferCopy copy_region{};
    copy_region.srcOffset = slice.offset;
    copy_region.dstOffset = dst_offset;
    copy_region.size = slice.size;
    slot.cmd_buffer.copyBuffer(slice.buffer, dst_buffer, copy_region);
}

void StagingManager::RecordBufferToImage(const StagingSlice& slice,
                                          vk::Image dst_image, uint32_t width, uint32_t height) {
    if (slice.slot_index >= slots_.size()) return;
    auto& slot = slots_[slice.slot_index];
    if (!*slot.cmd_buffer) return;

    vk::BufferImageCopy copy_region{};
    copy_region.bufferOffset = slice.offset;
    copy_region.bufferRowLength = 0;
    copy_region.bufferImageHeight = 0;
    copy_region.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
    copy_region.imageSubresource.mipLevel = 0;
    copy_region.imageSubresource.baseArrayLayer = 0;
    copy_region.imageSubresource.layerCount = 1;
    copy_region.imageOffset = vk::Offset3D{0, 0, 0};
    copy_region.imageExtent = vk::Extent3D{width, height, 1};
    slot.cmd_buffer.copyBufferToImage(slice.buffer, dst_image,
                                       vk::ImageLayout::eTransferDstOptimal, copy_region);
}

void StagingManager::Flush() {
    if (!backend_) return;

    bool has_work = false;
    for (auto& slot : slots_) {
        if (!slot.busy) continue;

        slot.cmd_buffer.end();

        vk::SubmitInfo submit{};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &*slot.cmd_buffer;

        backend_->GetGraphicsQueue().submit(submit, *slot.fence);
        has_work = true;
    }

    if (!has_work) return;
}

void StagingManager::WaitForSlot(uint32_t slot_index) {
    if (slot_index >= slots_.size()) return;
    auto& slot = slots_[slot_index];
    if (!slot.busy) return;

    if (*slot.fence) {
        const auto result = backend_->GetDevice().waitForFences(*slot.fence, vk::True, UINT64_T_MAX);
        if (result != vk::Result::eSuccess) {
            throw std::runtime_error("StagingManager: WaitForSlot fence wait failed");
        }
    }
    slot.busy = false;
}

void StagingManager::WaitForAll() {
    if (!backend_) return;

    for (auto& slot : slots_) {
        if (!slot.busy) continue;
        if (*slot.fence) {
            const auto result = backend_->GetDevice().waitForFences(*slot.fence, vk::True, UINT64_T_MAX);
            if (result != vk::Result::eSuccess) {
                throw std::runtime_error("StagingManager: WaitForAll fence wait failed");
            }
        }
        slot.busy = false;
    }
}

} // namespace VulkanEngine::GpuResources
