module;

#include <cstdint>
#include <vector>

#include <vulkan/vulkan_raii.hpp>

#include <logging/logging.hpp>

module VulkanEngine.GpuResources.HostRingPool;

import VulkanBackend.Runtime.VulkanBootstrap;
import VulkanEngine.GpuBuffer;

namespace VulkanEngine::GpuResources {

HostRingPool::~HostRingPool() {
    Shutdown();
}

bool HostRingPool::Initialize(VulkanEngine::Runtime::IVulkanBootstrap& backend,
                               const Config& config) {
    config_ = config;
    if (config_.frames_in_flight == 0) return false;

    const vk::MemoryPropertyFlags mem_flags =
        vk::MemoryPropertyFlagBits::eHostVisible |
        vk::MemoryPropertyFlagBits::eHostCoherent;

    frames_.reserve(config_.frames_in_flight);
    for (uint32_t i = 0; i < config_.frames_in_flight; ++i) {
        FrameData fd;
        fd.vertex_buffer = GpuBuffer::Create(
            backend, config_.vertex_buffer_size,
            vk::BufferUsageFlagBits::eStorageBuffer |
            vk::BufferUsageFlagBits::eTransferDst,
            mem_flags);
        if (!fd.vertex_buffer.IsValid()) {
            LOGIFACE_LOG(error, "HostRingPool: failed to create vertex buffer for frame " +
                         std::to_string(i));
            frames_.clear();
            return false;
        }
        fd.vertex_mapping = fd.vertex_buffer.Map(0, config_.vertex_buffer_size);

        fd.index_buffer = GpuBuffer::Create(
            backend, config_.index_buffer_size,
            vk::BufferUsageFlagBits::eStorageBuffer |
            vk::BufferUsageFlagBits::eTransferDst,
            mem_flags);
        if (!fd.index_buffer.IsValid()) {
            LOGIFACE_LOG(error, "HostRingPool: failed to create index buffer for frame " +
                         std::to_string(i));
            frames_.clear();
            return false;
        }
        fd.index_mapping = fd.index_buffer.Map(0, config_.index_buffer_size);

        frames_.push_back(std::move(fd));
    }

    next_vertex_offset_ = 0;
    next_index_offset_ = 0;

    return true;
}

void HostRingPool::Shutdown() {
    frames_.clear();
    handles_.clear();
    next_vertex_offset_ = 0;
    next_index_offset_ = 0;
}

HostRingPool::StreamedMeshHandle HostRingPool::RegisterStreamedMesh(
    uint64_t max_vertex_bytes, uint64_t max_index_bytes) {
    StreamedMeshHandle handle{};

    // Align to 256 bytes (common GPU requirement)
    constexpr uint64_t alignment = 256ULL;

    const uint64_t aligned_vertex = (next_vertex_offset_ + alignment - 1) & ~(alignment - 1);
    const uint64_t aligned_index = (next_index_offset_ + alignment - 1) & ~(alignment - 1);

    if (aligned_vertex + max_vertex_bytes > config_.vertex_buffer_size ||
        aligned_index + max_index_bytes > config_.index_buffer_size) {
        LOGIFACE_LOG(error, "HostRingPool: out of space for streamed mesh registration");
        return handle;
    }

    handle.id = static_cast<uint32_t>(handles_.size());
    handle.vertex_offset = aligned_vertex;
    handle.vertex_max_size = max_vertex_bytes;
    handle.index_offset = aligned_index;
    handle.index_max_size = max_index_bytes;

    handles_.push_back(handle);

    next_vertex_offset_ = aligned_vertex + max_vertex_bytes;
    next_index_offset_ = aligned_index + max_index_bytes;

    return handle;
}

void* HostRingPool::MapVertexData(StreamedMeshHandle handle, uint32_t frame_index) const {
    if (!handle.IsValid() || frame_index >= frames_.size()) return nullptr;
    const auto& frame = frames_[frame_index];
    if (!frame.vertex_mapping) return nullptr;
    return static_cast<char*>(frame.vertex_mapping) + handle.vertex_offset;
}

void* HostRingPool::MapIndexData(StreamedMeshHandle handle, uint32_t frame_index) const {
    if (!handle.IsValid() || frame_index >= frames_.size()) return nullptr;
    const auto& frame = frames_[frame_index];
    if (!frame.index_mapping) return nullptr;
    return static_cast<char*>(frame.index_mapping) + handle.index_offset;
}

vk::Buffer HostRingPool::GetVertexBuffer(uint32_t frame_index) const {
    if (frame_index >= frames_.size()) return VK_NULL_HANDLE;
    return *frames_[frame_index].vertex_buffer.GetBuffer();
}

vk::Buffer HostRingPool::GetIndexBuffer(uint32_t frame_index) const {
    if (frame_index >= frames_.size()) return VK_NULL_HANDLE;
    return *frames_[frame_index].index_buffer.GetBuffer();
}

} // namespace VulkanEngine::GpuResources
