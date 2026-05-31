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
                              VulkanEngine::GpuResources::DeviceBufferHeap* dynamic_vertex_heaps,
                              VulkanEngine::GpuResources::DeviceBufferHeap* dynamic_index_heaps,
                              uint32_t frames_in_flight) {
    if (!vertex_heap || !index_heap || !staging_mgr || !dynamic_vertex_heaps || !dynamic_index_heaps) {
        LOGIFACE_LOG(error, "MeshManager: null dependencies");
        return false;
    }
    backend_ = &backend;
    vertex_heap_ = vertex_heap;
    index_heap_ = index_heap;
    staging_mgr_ = staging_mgr;
    dynamic_vertex_heaps_ = dynamic_vertex_heaps;
    dynamic_index_heaps_ = dynamic_index_heaps;
    frames_in_flight_ = frames_in_flight;
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

    if (!dynamic_vertex_heaps_ || !dynamic_index_heaps_) return handle;

    const uint64_t vertex_bytes = initial_data.vertices.size() *
        sizeof(VulkanEngine::StandardMeshPipeline::Vertex);
    const uint64_t index_bytes = initial_data.indices.size() * sizeof(uint32_t);

    if (vertex_bytes == 0 || index_bytes == 0) {
        LOGIFACE_LOG(warn, "RegisterStreamed: empty mesh data (" +
                     std::to_string(initial_data.vertices.size()) + " verts, " +
                     std::to_string(initial_data.indices.size()) + " idxs)");
        return handle;
    }

    LOGIFACE_LOG(trace, "RegisterStreamed: " + std::to_string(initial_data.vertices.size()) +
                 " verts (" + std::to_string(vertex_bytes) + " bytes), " +
                 std::to_string(initial_data.indices.size()) + " idxs (" +
                 std::to_string(index_bytes) + " bytes), " +
                 std::to_string(initial_data.sub_meshes.size()) + " submeshes, " +
                 std::to_string(frames_in_flight_) + " FIFs");

    constexpr uint64_t alignment = 256ULL;

    GpuMeshInfo info{};
    info.sub_meshes = initial_data.sub_meshes;

    for (uint32_t fif = 0; fif < frames_in_flight_; ++fif) {
        auto& vtx_alloc = info.streamed_vertex_alloc[fif];
        auto& idx_alloc = info.streamed_index_alloc[fif];

        vtx_alloc = dynamic_vertex_heaps_[fif].Allocate(vertex_bytes, alignment);
        if (!vtx_alloc.IsValid()) {
            for (uint32_t j = 0; j < fif; ++j) {
                if (info.streamed_vertex_alloc[j].IsValid())
                    dynamic_vertex_heaps_[j].Free(info.streamed_vertex_alloc[j]);
                if (info.streamed_index_alloc[j].IsValid())
                    dynamic_index_heaps_[j].Free(info.streamed_index_alloc[j]);
            }
            LOGIFACE_LOG(error, "MeshManager::RegisterStreamed: vertex allocation failed for FIF " +
                         std::to_string(fif));
            return handle;
        }

        idx_alloc = dynamic_index_heaps_[fif].Allocate(index_bytes, alignment);
        if (!idx_alloc.IsValid()) {
            dynamic_vertex_heaps_[fif].Free(vtx_alloc);
            for (uint32_t j = 0; j < fif; ++j) {
                if (info.streamed_vertex_alloc[j].IsValid())
                    dynamic_vertex_heaps_[j].Free(info.streamed_vertex_alloc[j]);
                if (info.streamed_index_alloc[j].IsValid())
                    dynamic_index_heaps_[j].Free(info.streamed_index_alloc[j]);
            }
            LOGIFACE_LOG(error, "MeshManager::RegisterStreamed: index allocation failed for FIF " +
                         std::to_string(fif));
            return handle;
        }

        LOGIFACE_LOG(trace, "RegisterStreamed: FIF " + std::to_string(fif) +
                     " vtx_alloc buf_idx=" + std::to_string(vtx_alloc.buffer_index) +
                     " offset=" + std::to_string(vtx_alloc.offset) +
                     " size=" + std::to_string(vtx_alloc.size) +
                     " idx_alloc buf_idx=" + std::to_string(idx_alloc.buffer_index) +
                     " offset=" + std::to_string(idx_alloc.offset) +
                     " size=" + std::to_string(idx_alloc.size));

        void* vptr = dynamic_vertex_heaps_[fif].MapBuffer(
            vtx_alloc.buffer_index, vtx_alloc.offset);
        void* iptr = dynamic_index_heaps_[fif].MapBuffer(
            idx_alloc.buffer_index, idx_alloc.offset);
        if (vptr) std::memcpy(vptr, initial_data.vertices.data(), vertex_bytes);
        if (iptr) std::memcpy(iptr, initial_data.indices.data(), index_bytes);
    }

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
    entry.info = std::move(info);

    handle.id = handle_id;
    return handle;
}

void MeshManager::UpdateStreamed(Handle handle,
                                  const VulkanEngine::GpuResources::MeshData& data,
                                  uint32_t frame_index) {
    if (!handle.IsValid() || handle.id >= entries_.size()) {
        LOGIFACE_LOG(debug, "UpdateStreamed: invalid handle (id=" +
                     std::to_string(handle.id) + "), skipping");
        return;
    }

    const auto& entry = entries_[handle.id];
    if (entry.strategy != Strategy::Streamed) {
        LOGIFACE_LOG(debug, "UpdateStreamed: handle " + std::to_string(handle.id) +
                     " is not Streamed, skipping");
        return;
    }

    const uint32_t fif = frame_index % frames_in_flight_;
    const auto& vtx_alloc = entry.info.streamed_vertex_alloc[fif];
    const auto& idx_alloc = entry.info.streamed_index_alloc[fif];

    if (!vtx_alloc.IsValid() || !idx_alloc.IsValid()) {
        LOGIFACE_LOG(debug, "UpdateStreamed: FIF " + std::to_string(fif) +
                     " streamed alloc invalid for handle " + std::to_string(handle.id) +
                     ", skipping");
        return;
    }

    const uint64_t vertex_bytes = data.vertices.size() *
        sizeof(VulkanEngine::StandardMeshPipeline::Vertex);
    const uint64_t index_bytes = data.indices.size() * sizeof(uint32_t);

    LOGIFACE_LOG(trace, "UpdateStreamed: handle=" + std::to_string(handle.id) +
                 " fif=" + std::to_string(fif) +
                 " vtx buf_idx=" + std::to_string(vtx_alloc.buffer_index) +
                 " offset=" + std::to_string(vtx_alloc.offset) +
                 " bytes=" + std::to_string(vertex_bytes) +
                 " idx buf_idx=" + std::to_string(idx_alloc.buffer_index) +
                 " offset=" + std::to_string(idx_alloc.offset) +
                 " bytes=" + std::to_string(index_bytes));

    void* vptr = dynamic_vertex_heaps_[fif].MapBuffer(
        vtx_alloc.buffer_index, vtx_alloc.offset);
    if (!vptr) {
        LOGIFACE_LOG(warn, "UpdateStreamed: MapBuffer returned null for vertex heap " +
                     std::to_string(fif) + " block " + std::to_string(vtx_alloc.buffer_index));
    } else {
        std::memcpy(vptr, data.vertices.data(), vertex_bytes);
    }

    void* iptr = dynamic_index_heaps_[fif].MapBuffer(
        idx_alloc.buffer_index, idx_alloc.offset);
    if (!iptr) {
        LOGIFACE_LOG(warn, "UpdateStreamed: MapBuffer returned null for index heap " +
                     std::to_string(fif) + " block " + std::to_string(idx_alloc.buffer_index));
    } else {
        std::memcpy(iptr, data.indices.data(), index_bytes);
    }
}

void MeshManager::Remove(Handle handle) {
    if (!handle.IsValid() || handle.id >= entries_.size()) return;

    MeshEntry& entry = entries_[handle.id];
    const int32_t frames_wait = static_cast<int32_t>(frames_in_flight_);

    if (entry.strategy == Strategy::Persistent) {
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
    } else if (entry.strategy == Strategy::Streamed) {
        for (uint32_t fif = 0; fif < frames_in_flight_; ++fif) {
            auto& valloc = entry.info.streamed_vertex_alloc[fif];
            auto& ialloc = entry.info.streamed_index_alloc[fif];
            if (valloc.IsValid()) {
                deferred_free_queue_.push_back({
                    valloc, &dynamic_vertex_heaps_[fif], frames_wait
                });
            }
            if (ialloc.IsValid()) {
                deferred_free_queue_.push_back({
                    ialloc, &dynamic_index_heaps_[fif], frames_wait
                });
            }
        }
    }

    entry = {};
    free_handles_.push_back(handle.id);
}

void MeshManager::EndFrame(uint32_t) {
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

vk::Buffer MeshManager::GetDynamicVertexBuffer(uint32_t fif_index, uint32_t buffer_index) const {
    if (fif_index >= frames_in_flight_) return VK_NULL_HANDLE;
    return dynamic_vertex_heaps_[fif_index].GetBuffer(buffer_index);
}

vk::Buffer MeshManager::GetDynamicIndexBuffer(uint32_t fif_index, uint32_t buffer_index) const {
    if (fif_index >= frames_in_flight_) return VK_NULL_HANDLE;
    return dynamic_index_heaps_[fif_index].GetBuffer(buffer_index);
}

uint64_t MeshManager::GetDynamicVertexBlockSize(uint32_t fif_index) const {
    if (fif_index >= frames_in_flight_) return 0;
    return dynamic_vertex_heaps_[fif_index].GetConfig().block_size;
}

uint64_t MeshManager::GetDynamicIndexBlockSize(uint32_t fif_index) const {
    if (fif_index >= frames_in_flight_) return 0;
    return dynamic_index_heaps_[fif_index].GetConfig().block_size;
}

uint32_t MeshManager::GetDynamicVertexBlockCount(uint32_t fif_index) const {
    if (fif_index >= frames_in_flight_) return 0;
    return dynamic_vertex_heaps_[fif_index].GetBufferCount();
}

uint32_t MeshManager::GetDynamicIndexBlockCount(uint32_t fif_index) const {
    if (fif_index >= frames_in_flight_) return 0;
    return dynamic_index_heaps_[fif_index].GetBufferCount();
}

} // namespace VulkanEngine
