module;

#include <cstdint>
#include <vector>
#include <cstring>

#include <vulkan/vulkan_raii.hpp>

#include <logging/logging.hpp>

module VulkanEngine.MeshManager;

import VulkanBackend.Runtime.VulkanBootstrap;
import VulkanEngine.GpuResources.DeviceBufferHeap;
import VulkanEngine.GpuResources.StagingManager;
import VulkanEngine.GpuResources.HostRingPool;
import VulkanEngine.GpuResources.MeshData;
import VulkanEngine.StandardMeshPipeline;

namespace VulkanEngine {

MeshManager::~MeshManager() {
    Shutdown();
}

bool MeshManager::Initialize(VulkanEngine::Runtime::IVulkanBootstrap& backend,
                              VulkanEngine::GpuResources::DeviceBufferHeap* vertex_heap,
                              VulkanEngine::GpuResources::DeviceBufferHeap* index_heap,
                              VulkanEngine::GpuResources::StagingManager* staging_mgr,
                              VulkanEngine::GpuResources::HostRingPool* ring_pool) {
    if (!vertex_heap || !index_heap || !staging_mgr || !ring_pool) {
        LOGIFACE_LOG(error, "MeshManager: null dependencies");
        return false;
    }
    backend_ = &backend;
    vertex_heap_ = vertex_heap;
    index_heap_ = index_heap;
    staging_mgr_ = staging_mgr;
    ring_pool_ = ring_pool;
    frames_in_flight_ = ring_pool->GetFramesInFlight();
    return true;
}

void MeshManager::Shutdown() {
    entries_.clear();
    free_handles_.clear();
    deferred_free_queue_.clear();
    backend_ = nullptr;
}

MeshManager::Handle MeshManager::UploadPersistent(
    const VulkanEngine::GpuResources::MeshData& data) {
    Handle handle{};

    if (!backend_ || data.vertices.empty() || data.indices.empty()) {
        LOGIFACE_LOG(warn, "MeshManager::UploadPersistent: empty data");
        return handle;
    }

    const uint64_t vertex_data_size = data.vertices.size() *
        sizeof(VulkanEngine::StandardMeshPipeline::Vertex);
    const uint64_t index_data_size = data.indices.size() * sizeof(uint32_t);

    // Allocate from device-local heaps
    constexpr uint64_t alignment = 256ULL;
    auto vertex_alloc = vertex_heap_->Allocate(vertex_data_size, alignment);
    if (!vertex_alloc.IsValid()) {
        LOGIFACE_LOG(error, "MeshManager::UploadPersistent: vertex heap allocation failed");
        return handle;
    }

    auto index_alloc = index_heap_->Allocate(index_data_size, alignment);
    if (!index_alloc.IsValid()) {
        vertex_heap_->Free(vertex_alloc);
        LOGIFACE_LOG(error, "MeshManager::UploadPersistent: index heap allocation failed");
        return handle;
    }

    // Upload vertices via staging
    {
        auto slice = staging_mgr_->Allocate(vertex_data_size);
        if (!slice.data) {
            vertex_heap_->Free(vertex_alloc);
            index_heap_->Free(index_alloc);
            LOGIFACE_LOG(error, "MeshManager::UploadPersistent: vertex staging allocation failed");
            return handle;
        }
        std::memcpy(slice.data, data.vertices.data(), vertex_data_size);
        staging_mgr_->RecordBufferCopy(slice,
            vertex_heap_->GetBuffer(vertex_alloc.buffer_index), vertex_alloc.offset);
    }

    // Upload raw indices via staging — no packing, no base-vertex adjustment.
    // The GPU will add baseVertex from StaticEntry.vertexInfo in the expand shader.
    {
        auto slice = staging_mgr_->Allocate(index_data_size);
        if (!slice.data) {
            vertex_heap_->Free(vertex_alloc);
            index_heap_->Free(index_alloc);
            LOGIFACE_LOG(error, "MeshManager::UploadPersistent: index staging allocation failed");
            return handle;
        }
        std::memcpy(slice.data, data.indices.data(), index_data_size);
        staging_mgr_->RecordBufferCopy(slice,
            index_heap_->GetBuffer(index_alloc.buffer_index), index_alloc.offset);
    }

    staging_mgr_->Flush();
    staging_mgr_->WaitForAll();

    // Assign handle
    uint32_t handle_id;
    if (!free_handles_.empty()) {
        handle_id = free_handles_.back();
        free_handles_.pop_back();
        entries_[handle_id] = {};
    } else {
        handle_id = static_cast<uint32_t>(entries_.size());
        entries_.emplace_back();
    }

    MeshEntry& entry = entries_[handle_id];
    entry.strategy = Strategy::Persistent;
    entry.info.vertex_allocation = vertex_alloc;
    entry.info.index_allocation = index_alloc;
    entry.info.vertex_buffer_index = vertex_alloc.buffer_index;
    entry.info.index_buffer_index = index_alloc.buffer_index;
    entry.info.sub_meshes = data.sub_meshes;

    handle.id = handle_id;
    return handle;
}

MeshManager::Handle MeshManager::RegisterStreamed(
    const VulkanEngine::GpuResources::MeshData& initial_data) {
    Handle handle{};

    if (!ring_pool_) return handle;

    const uint64_t vertex_bytes = initial_data.vertices.size() *
        sizeof(VulkanEngine::StandardMeshPipeline::Vertex);
    const uint64_t index_bytes = initial_data.indices.size() * sizeof(uint32_t);

    if (vertex_bytes == 0 || index_bytes == 0) return handle;

    auto stream_handle = ring_pool_->RegisterStreamedMesh(vertex_bytes, index_bytes);
    if (!stream_handle.IsValid()) {
        LOGIFACE_LOG(error, "MeshManager::RegisterStreamed: ring pool registration failed");
        return handle;
    }

    // Write initial data to all ring slots
    const uint32_t frames_in_flight = ring_pool_->GetFramesInFlight();
    for (uint32_t fi = 0; fi < frames_in_flight; ++fi) {
        void* vptr = ring_pool_->MapVertexData(stream_handle, fi);
        void* iptr = ring_pool_->MapIndexData(stream_handle, fi);
        if (vptr) std::memcpy(vptr, initial_data.vertices.data(), vertex_bytes);
        if (iptr) std::memcpy(iptr, initial_data.indices.data(), index_bytes);
    }

    // Assign handle
    uint32_t handle_id;
    if (!free_handles_.empty()) {
        handle_id = free_handles_.back();
        free_handles_.pop_back();
        entries_[handle_id] = {};
    } else {
        handle_id = static_cast<uint32_t>(entries_.size());
        entries_.emplace_back();
    }

    MeshEntry& entry = entries_[handle_id];
    entry.strategy = Strategy::Streamed;
    entry.stream_handle = stream_handle;
    entry.info.sub_meshes = initial_data.sub_meshes;
    entry.info.vertex_buffer_index = stream_handle.id;
    entry.info.index_buffer_index = stream_handle.id;
    entry.info.vertex_allocation.offset = stream_handle.vertex_offset;
    entry.info.index_allocation.offset = stream_handle.index_offset;

    handle.id = handle_id;
    return handle;
}

void MeshManager::UpdateStreamed(Handle handle,
                                  const VulkanEngine::GpuResources::MeshData& data,
                                  uint32_t frame_index) {
    if (!handle.IsValid() || handle.id >= entries_.size()) return;

    const auto& entry = entries_[handle.id];
    if (entry.strategy != Strategy::Streamed) return;

    const uint64_t vertex_bytes = data.vertices.size() *
        sizeof(VulkanEngine::StandardMeshPipeline::Vertex);
    const uint64_t index_bytes = data.indices.size() * sizeof(uint32_t);

    void* vptr = ring_pool_->MapVertexData(entry.stream_handle, frame_index);
    void* iptr = ring_pool_->MapIndexData(entry.stream_handle, frame_index);

    if (vptr) std::memcpy(vptr, data.vertices.data(), vertex_bytes);
    if (iptr) std::memcpy(iptr, data.indices.data(), index_bytes);
}

void MeshManager::Remove(Handle handle) {
    if (!handle.IsValid() || handle.id >= entries_.size()) return;

    MeshEntry& entry = entries_[handle.id];
    if (entry.strategy == Strategy::Persistent) {
        const int32_t frames_wait = static_cast<int32_t>(frames_in_flight_);
        if (entry.info.vertex_allocation.IsValid()) {
            deferred_free_queue_.push_back({
                entry.info.vertex_allocation, vertex_heap_, frames_wait
            });
        }
        if (entry.info.index_allocation.IsValid()) {
            deferred_free_queue_.push_back({
                entry.info.index_allocation, index_heap_, frames_wait
            });
        }
    }

    entry = {};
    free_handles_.push_back(handle.id);
}

void MeshManager::EndFrame(uint32_t /*frame_index*/) {
    // Drain deferred free queue by countdown
    auto it = deferred_free_queue_.begin();
    while (it != deferred_free_queue_.end()) {
        it->frames_remaining--;
        if (it->frames_remaining <= 0) {
            if (it->heap && it->allocation.IsValid()) {
                auto alloc = it->allocation;
                it->heap->Free(alloc);
            }
            it = deferred_free_queue_.erase(it);
        } else {
            ++it;
        }
    }
}

const MeshManager::GpuMeshInfo* MeshManager::GetMeshInfo(Handle handle) const {
    if (!handle.IsValid() || handle.id >= entries_.size()) return nullptr;
    return &entries_[handle.id].info;
}

vk::Buffer MeshManager::GetStreamedVertexBuffer(uint32_t frame_index) const {
    if (!ring_pool_) return VK_NULL_HANDLE;
    return ring_pool_->GetVertexBuffer(frame_index);
}

vk::Buffer MeshManager::GetStreamedIndexBuffer(uint32_t frame_index) const {
    if (!ring_pool_) return VK_NULL_HANDLE;
    return ring_pool_->GetIndexBuffer(frame_index);
}

uint64_t MeshManager::GetStreamedVertexBufferSize() const {
    return ring_pool_ ? ring_pool_->GetVertexBufferSize() : 0;
}

uint64_t MeshManager::GetStreamedIndexBufferSize() const {
    return ring_pool_ ? ring_pool_->GetIndexBufferSize() : 0;
}

} // namespace VulkanEngine
