module;

#include <logging/logging_macros.hpp>

module VulkanEngine.MeshRegistry;

import std;
import std.compat;

import logiface;

import vulkan;

import VulkanEngine.GpuResources.MeshData;
import VulkanEngine.MeshManager;
import VulkanEngine.SceneRenderer;
import VulkanEngine.GpuResources.DeviceBufferHeap;

namespace VulkanEngine {

bool TablePublishRecord::Note(const SceneGpuLayout::TableId table, const std::uint32_t index) {
    const auto slot = static_cast<std::size_t>(table);
    if (slot >= seen_.size()) {
        return false;
    }
    return seen_[slot].insert(index).second;
}

bool TablePublishRecord::Contains(const SceneGpuLayout::TableId table, const std::uint32_t index) const {
    const auto slot = static_cast<std::size_t>(table);
    if (slot >= seen_.size()) {
        return false;
    }
    return seen_[slot].contains(index);
}

void TablePublishRecord::Clear() {
    for (auto& seen : seen_) {
        seen.clear();
    }
}

MeshRegistry::~MeshRegistry() {
    Shutdown();
}

MeshHandle MeshRegistry::Register(const GpuResources::MeshData& data) {
    std::uint32_t slot;
    if (!free_slots_.empty()) {
        slot = free_slots_.back();
        free_slots_.pop_back();
    } else {
        slot = static_cast<std::uint32_t>(entries_.size());
        entries_.emplace_back();
    }

    auto normalized = data;
    VulkanEngine::GpuResources::EnsureSubmeshBounds(normalized);

    auto& info = entries_[slot];
    // A reused slot keeps its generation and only its payload is reset, because clearing the whole entry would resurrect every stale handle for this slot.
    info.cpu_data = std::move(normalized);
    info.gpu_handle = {};
    info.first_submesh_in_renderer = 0;
    info.submesh_count = static_cast<std::uint32_t>(
        info.cpu_data.sub_meshes.empty() ? 1 : info.cpu_data.sub_meshes.size());
    info.alive = true;
    info.pending_upload = false;
    info.gpu_resident = false;
    info.frames_since_last_reference = 0;
    // The counter is 32 bits, so one slot wraps only after four billion registrations, which a frame cannot reach.
    info.generation += 1;

    return MeshHandle{slot, info.generation};
}

LoadedMeshInfo* MeshRegistry::Find(const MeshHandle handle) {
    if (handle.slot >= entries_.size()) return nullptr;
    LoadedMeshInfo& info = entries_[handle.slot];
    if (!info.alive || info.generation != handle.generation) return nullptr;
    return &info;
}

const LoadedMeshInfo* MeshRegistry::Find(const MeshHandle handle) const {
    if (handle.slot >= entries_.size()) return nullptr;
    const LoadedMeshInfo& info = entries_[handle.slot];
    if (!info.alive || info.generation != handle.generation) return nullptr;
    return &info;
}

void MeshRegistry::RequestGpuResidency(const MeshHandle mesh_id, MeshManager& mgr,
                                        SceneRenderer::SceneRenderer& renderer,
                                        GpuResources::DeviceBufferHeap& vtx_heap,
                                        GpuResources::DeviceBufferHeap& idx_heap,
                                        GpuResources::DeviceBufferHeap& uv_heap) {
    LoadedMeshInfo* const found = Find(mesh_id);
    if (found == nullptr) return;

    auto& info = *found;

    // Mark as referenced this frame
    referenced_this_frame_.insert(mesh_id.slot);

    if (info.gpu_resident) {
        info.frames_since_last_reference = 0;
        return;
    }

    if (info.pending_upload) return;

    if (info.cpu_data.vertices.empty()) return;

    // Upload to GPU
    info.pending_upload = true;
    auto handle = mgr.UploadPersistent(info.cpu_data);
    if (!handle.IsValid()) {
        info.pending_upload = false;
        LOGIFACE_LOG(error, "MeshRegistry: upload failed for mesh " + std::to_string(mesh_id.slot));
        return;
    }

    info.gpu_handle = handle;
    auto* gpu_info = mgr.GetMeshInfo(handle);
    if (!gpu_info) {
        info.pending_upload = false;
        return;
    }

    // Add submeshes to renderer with adjusted index_start
    const std::uint32_t index_offset = static_cast<std::uint32_t>(
        gpu_info->index_allocation.offset / sizeof(std::uint32_t));

    const std::vector<SubMesh> adjusted = AdjustSubmeshesForIndexBlock(
        gpu_info->sub_meshes, index_offset,
        static_cast<std::uint32_t>(gpu_info->index_allocation.size / sizeof(std::uint32_t)));

    info.first_submesh_in_renderer = renderer.AppendSubmeshes(adjusted);
    info.submesh_count = static_cast<std::uint32_t>(adjusted.size());

    // One descriptor per heap block. Several meshes that land in the same block in one frame would otherwise write it once each.
    const auto publish = [&](const SceneGpuLayout::TableId table,
                             const std::uint32_t block_index,
                             const vk::Buffer buffer,
                             const std::uint64_t block_size) {
        if (!buffer) return;
        if (!published_this_frame_.Note(table, block_index)) return;
        renderer.UpdateTableElement(table, std::nullopt, block_index, buffer, block_size);
    };

    if (gpu_info->vertex_allocation.buffer_index < vtx_heap.GetBufferCount()) {
        publish(SceneGpuLayout::TableId::vertex, gpu_info->vertex_allocation.buffer_index,
                vtx_heap.GetBuffer(gpu_info->vertex_allocation.buffer_index),
                vtx_heap.GetConfig().block_size);
    }
    if (gpu_info->index_allocation.buffer_index < idx_heap.GetBufferCount()) {
        publish(SceneGpuLayout::TableId::index, gpu_info->index_allocation.buffer_index,
                idx_heap.GetBuffer(gpu_info->index_allocation.buffer_index),
                idx_heap.GetConfig().block_size);
    }
    // UV1 lives in its own heap, so its block needs the uv table just as the geometry needs the vertex and index tables.
    if (gpu_info->has_uv1 && gpu_info->uv_allocation.buffer_index < uv_heap.GetBufferCount()) {
        publish(SceneGpuLayout::TableId::uv, gpu_info->uv_allocation.buffer_index,
                uv_heap.GetBuffer(gpu_info->uv_allocation.buffer_index),
                uv_heap.GetConfig().block_size);
    }
    // Skin-influence tables. The deform set outlives capacity growth, so a block
    // that becomes resident while frames are in flight still has to be published
    // to every frame.
    if (gpu_info->influence_sets > 0) {
        publish(SceneGpuLayout::TableId::skin, gpu_info->skin_allocation.buffer_index,
                mgr.GetSkinBuffer(gpu_info->skin_allocation.buffer_index), mgr.GetSkinBlockSize());
    }
    if (gpu_info->has_extra_skin) {
        publish(SceneGpuLayout::TableId::skin_extra, gpu_info->skin_extra_allocation.buffer_index,
                mgr.GetSkinExtraBuffer(gpu_info->skin_extra_allocation.buffer_index), mgr.GetSkinBlockSize());
    }

    info.gpu_resident = true;
    info.pending_upload = false;
    info.frames_since_last_reference = 0;
}

void MeshRegistry::Release(const MeshHandle mesh_id, MeshManager& mgr) {
    LoadedMeshInfo* const found = Find(mesh_id);
    if (found == nullptr) return;

    auto& info = *found;
    if (info.gpu_resident) {
        mgr.Remove(info.gpu_handle);
        info.gpu_resident = false;
        info.gpu_handle = {};
    }

    // The slot goes back on the free list only after its payload is gone.
    // It keeps its generation, which is what stops this handle from releasing the mesh that reuses the slot.
    info.alive = false;
    info.cpu_data = {};
    info.submesh_count = 0;
    free_slots_.push_back(mesh_id.slot);
    referenced_this_frame_.erase(mesh_id.slot);
}

void MeshRegistry::EndFrame(MeshManager& mgr, std::uint32_t eviction_timeout_frames) {
    for (std::uint32_t id = 0; id < static_cast<std::uint32_t>(entries_.size()); ++id) {
        auto& info = entries_[id];

        // Skip free slots
        if (!info.alive) continue;

        if (referenced_this_frame_.count(id) == 0) {
            info.frames_since_last_reference++;
            if (info.frames_since_last_reference >= eviction_timeout_frames && info.gpu_resident) {
                mgr.Remove(info.gpu_handle);
                info.gpu_resident = false;
                // Submeshes stay in the renderer's list with zeroed index_count
            }
        } else {
            info.frames_since_last_reference = 0;
        }
    }
    referenced_this_frame_.clear();
    // Cleared every frame, so the first publish of a block in a later frame is never suppressed.
    published_this_frame_.Clear();
}

const LoadedMeshInfo* MeshRegistry::Get(const MeshHandle mesh_id) const {
    return Find(mesh_id);
}

void MeshRegistry::Shutdown() {
    entries_.clear();
    free_slots_.clear();
    referenced_this_frame_.clear();
    published_this_frame_.Clear();
}

}

