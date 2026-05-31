module;

#include <cstdint>
#include <vector>
#include <unordered_set>

#include <logging/logging.hpp>

module VulkanEngine.MeshRegistry;

import VulkanEngine.GpuResources.MeshData;
import VulkanEngine.MeshManager;
import VulkanEngine.SceneRenderer;
import VulkanEngine.GpuResources.DeviceBufferHeap;

namespace VulkanEngine {

MeshRegistry::~MeshRegistry() {
    Shutdown();
}

uint32_t MeshRegistry::Register(const GpuResources::MeshData& data) {
    uint32_t id;
    if (!free_ids_.empty()) {
        id = free_ids_.back();
        free_ids_.pop_back();
        entries_[id] = {};
    } else {
        id = static_cast<uint32_t>(entries_.size());
        entries_.emplace_back();
    }

    auto& info = entries_[id];
    info.cpu_data = data;
    info.submesh_count = static_cast<uint32_t>(
        data.sub_meshes.empty() ? 1 : data.sub_meshes.size());

    return id;
}

void MeshRegistry::RequestGpuResidency(uint32_t mesh_id, MeshManager& mgr,
                                        SceneRenderer::SceneRenderer& renderer,
                                        GpuResources::DeviceBufferHeap& vtx_heap,
                                        GpuResources::DeviceBufferHeap& idx_heap) {
    if (mesh_id >= entries_.size()) return;

    auto& info = entries_[mesh_id];

    // Mark as referenced this frame
    referenced_this_frame_.insert(mesh_id);

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
        LOGIFACE_LOG(error, "MeshRegistry: upload failed for mesh " + std::to_string(mesh_id));
        return;
    }

    info.gpu_handle = handle;
    auto* gpu_info = mgr.GetMeshInfo(handle);
    if (!gpu_info) {
        info.pending_upload = false;
        return;
    }

    // Add submeshes to renderer with adjusted index_start
    const uint32_t index_offset = static_cast<uint32_t>(
        gpu_info->index_allocation.offset / sizeof(uint32_t));

    std::vector<SubMesh> adjusted;
    if (gpu_info->sub_meshes.empty()) {
        const uint32_t total_indices = static_cast<uint32_t>(
            gpu_info->index_allocation.size / sizeof(uint32_t));
        SubMesh sm{};
        sm.index_start = index_offset;
        sm.index_count = total_indices;
        adjusted.push_back(sm);
    } else {
        for (const auto& sm : gpu_info->sub_meshes) {
            auto copy = sm;
            copy.index_start += index_offset;
            adjusted.push_back(copy);
        }
    }

    info.first_submesh_in_renderer = static_cast<uint32_t>(
        renderer.GetSubmeshes().size());
    info.submesh_count = static_cast<uint32_t>(adjusted.size());

    // Append to renderer's submesh list
    auto all_submeshes = renderer.GetSubmeshes();
    all_submeshes.insert(all_submeshes.end(), adjusted.begin(), adjusted.end());
    renderer.SetSubmeshes(all_submeshes);

    // Update descriptor arrays if new heap block was allocated (all frames)
    if (gpu_info->vertex_allocation.buffer_index < vtx_heap.GetBufferCount()) {
        renderer.UpdateAllFrameVertexBufferArrayElements(
            gpu_info->vertex_allocation.buffer_index,
            vtx_heap.GetBuffer(gpu_info->vertex_allocation.buffer_index),
            vtx_heap.GetConfig().block_size);
    }
    if (gpu_info->index_allocation.buffer_index < idx_heap.GetBufferCount()) {
        renderer.UpdateAllFrameIndexBufferArrayElements(
            gpu_info->index_allocation.buffer_index,
            idx_heap.GetBuffer(gpu_info->index_allocation.buffer_index),
            idx_heap.GetConfig().block_size);
    }

    info.gpu_resident = true;
    info.pending_upload = false;
    info.frames_since_last_reference = 0;
}

void MeshRegistry::Release(uint32_t mesh_id, MeshManager& mgr) {
    if (mesh_id >= entries_.size()) return;

    auto& info = entries_[mesh_id];
    if (info.gpu_resident) {
        mgr.Remove(info.gpu_handle);
        info.gpu_resident = false;
        info.gpu_handle = {};
    }
    referenced_this_frame_.erase(mesh_id);
}

void MeshRegistry::EndFrame(MeshManager& mgr, uint32_t eviction_timeout_frames) {
    for (uint32_t id = 0; id < static_cast<uint32_t>(entries_.size()); ++id) {
        auto& info = entries_[id];

        // Skip free slots
        if (info.cpu_data.vertices.empty() && info.submesh_count == 0) continue;

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
}

const LoadedMeshInfo* MeshRegistry::Get(uint32_t mesh_id) const {
    if (mesh_id >= entries_.size()) return nullptr;
    return &entries_[mesh_id];
}

void MeshRegistry::Shutdown() {
    entries_.clear();
    free_ids_.clear();
    referenced_this_frame_.clear();
}

}

