module;

#include <cstdint>
#include <vector>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_RADIANS
#include <glm/glm.hpp>          // NOLINT(misc-include-cleaner)
#include <glm/gtc/quaternion.hpp> // NOLINT(misc-include-cleaner)

module VulkanEngine.MeshRenderSystem;

import VulkanBackend.Component;
import VulkanEngine.Components.Transform;
import VulkanEngine.Components.MeshReference;
import VulkanEngine.Components.DynamicMesh;
import VulkanEngine.MeshRegistry;
import VulkanEngine.MeshManager;
import VulkanEngine.SceneRenderer;
import VulkanEngine.GpuResources.DeviceBufferHeap;
import VulkanEngine.MaterialManager;
import VulkanEngine.BindlessManager;

namespace VulkanEngine {

void MeshRenderSystem::ProcessFrame(ComponentRegistry& registry,
                                     MeshRegistry& mesh_registry,
                                     MeshManager& mesh_mgr,
                                     SceneRenderer::SceneRenderer& renderer,
                                     GpuResources::DeviceBufferHeap& vtx_heap,
                                     GpuResources::DeviceBufferHeap& idx_heap,
                                     uint32_t frame_index) {
    // --- Phase 1: Collect static mesh entities ---
    std::vector<DrawEntity> static_ents;
    static_ents.reserve(MAX_GATHER);
    registry.ForEach<Components::MeshReference>(
        [&](Components::MeshReference& mr) {
            if (static_ents.size() >= MAX_GATHER) return;
            auto* owner = mr.GetOwner();
            if (!owner) return;
            auto* transform = owner->GetComponent<Components::Transform>();
            if (!transform) return;
            static_ents.push_back({ transform, &mr });
        });

    // --- Phase 2: Collect dynamic mesh entities ---
    std::vector<DynamicEntity> dyn_ents;
    dyn_ents.reserve(MAX_GATHER);
    registry.ForEach<Components::DynamicMesh>(
        [&](Components::DynamicMesh& dm) {
            if (dyn_ents.size() >= MAX_GATHER) return;
            auto* owner = dm.GetOwner();
            if (!owner) return;
            auto* transform = owner->GetComponent<Components::Transform>();
            if (!transform) return;
            dyn_ents.push_back({ transform, &dm });
        });

    // --- Phase 3: Request GPU residency for static meshes ---
    for (auto& e : static_ents) {
        const uint32_t mesh_id = e.mesh_ref->loaded_mesh_id;
        mesh_registry.RequestGpuResidency(mesh_id, mesh_mgr, renderer,
                                           vtx_heap, idx_heap);
    }

    // --- Phase 4: Count total submesh entries ---
    uint32_t total_submeshes = 0;

    for (auto& e : static_ents) {
        const auto* info = mesh_registry.Get(e.mesh_ref->loaded_mesh_id);
        if (!info || !info->gpu_resident) continue;
        total_submeshes += info->submesh_count;
    }

    {
        const uint32_t dyn_fif = frame_index % 3;
        for (auto& e : dyn_ents) {
            const auto* gpu_info = mesh_mgr.GetMeshInfo(e.dyn_mesh->gpu_handle);
            if (!gpu_info ||
                !gpu_info->streamed_vertex_alloc[dyn_fif].IsValid() ||
                !gpu_info->streamed_index_alloc[dyn_fif].IsValid()) continue;

            if (e.dyn_mesh->submesh_count > 0) {
                total_submeshes += e.dyn_mesh->submesh_count;
            } else {
                total_submeshes += 1;
            }
        }
    }

    // Update streamed data for dynamic meshes
    for (auto& e : dyn_ents) {
        mesh_mgr.UpdateStreamed(e.dyn_mesh->gpu_handle, e.dyn_mesh->mesh_data, frame_index);
    }

    // --- Phase 5: Write per-frame data to renderer block arrays ---
    auto frame_blocks = renderer.GetFrameBlockArrays(frame_index);
    auto& scene_submeshes = renderer.GetSubmeshes();

    frame_blocks.compact_dynamic->EnsureCapacity(total_submeshes);
    frame_blocks.compact_static->EnsureCapacity(total_submeshes);
    frame_blocks.bounding_spheres->EnsureCapacity(total_submeshes);
    frame_blocks.bounding_obb->EnsureCapacity(total_submeshes);

    struct alignas(16) DynamicEntry {
        float px, py, pz, pad0;
        float sx, sy, sz, pad1;
        float rx, ry, rz, rw;
    };
    struct alignas(16) StaticEntry {
        uint32_t index_start_packed;
        uint32_t index_range;
        uint32_t technique_texture;
        uint32_t vertex_info;
    };
    struct alignas(16) OBBGPUEntry {
        float cx, cy, cz, pad0;
        float ux, uy, uz, hu;
        float vx, vy, vz, hv;
        float wx, wy, wz, hw;
    };

    uint32_t ci = 0;

    // Write static mesh entries
    for (auto& e : static_ents) {
        const auto* loaded = mesh_registry.Get(e.mesh_ref->loaded_mesh_id);
        if (!loaded || !loaded->gpu_resident || !loaded->gpu_handle.IsValid()) continue;

        const auto* gpu_info = mesh_mgr.GetMeshInfo(loaded->gpu_handle);
        if (!gpu_info) continue;

        const uint32_t vertex_buf_slot = gpu_info->vertex_allocation.buffer_index;
        const uint32_t base_vertex = static_cast<uint32_t>(
            gpu_info->vertex_allocation.offset / sizeof(StandardMeshPipeline::Vertex));
        const uint32_t index_buf_slot = gpu_info->index_allocation.buffer_index;

        const auto pos = e.transform ? e.transform->position : glm::vec3(0);
        const auto scale = e.transform ? e.transform->scale : glm::vec3(1);
        const glm::quat rot = e.transform ? e.transform->rotation : glm::quat{1.0f, 0.0f, 0.0f, 0.0f};

        for (uint32_t s = 0; s < loaded->submesh_count; ++s) {
            const uint32_t si = loaded->first_submesh_in_renderer + s;
            const auto sm = (si < scene_submeshes.size())
                ? scene_submeshes[si]
                : SubMesh{};

            if (auto* d = static_cast<DynamicEntry*>(frame_blocks.compact_dynamic->Get(ci))) {
                d->px = pos.x; d->py = pos.y; d->pz = pos.z; d->pad0 = 0;
                d->sx = scale.x; d->sy = scale.y; d->sz = scale.z; d->pad1 = 0;
                d->rx = rot.x; d->ry = rot.y; d->rz = rot.z; d->rw = rot.w;
            }

            if (auto* s2 = static_cast<StaticEntry*>(frame_blocks.compact_static->Get(ci))) {
                s2->index_start_packed = (index_buf_slot << 24) | sm.index_start;
                s2->index_range = sm.index_count;
                {
                    const auto& mat_def = MaterialManager::MaterialManager::Get().GetMaterial(sm.material_id);
                    s2->technique_texture =
                        (static_cast<uint32_t>(mat_def.texture_slot.value) << 16) |
                        mat_def.technique_id.value;
                }
                s2->vertex_info = (vertex_buf_slot << 24) | base_vertex;
            }

            if (auto* sp = static_cast<glm::vec4*>(frame_blocks.bounding_spheres->Get(ci))) {
                sp->x = sm.sphere.center.x;
                sp->y = sm.sphere.center.y;
                sp->z = sm.sphere.center.z;
                sp->w = sm.sphere.radius;
            }

            if (auto* ob = static_cast<OBBGPUEntry*>(frame_blocks.bounding_obb->Get(ci))) {
                ob->cx = sm.obb.center.x; ob->cy = sm.obb.center.y; ob->cz = sm.obb.center.z; ob->pad0 = 0;
                ob->ux = sm.obb.axis_u.x; ob->uy = sm.obb.axis_u.y; ob->uz = sm.obb.axis_u.z; ob->hu = sm.obb.half_extent_u;
                ob->vx = sm.obb.axis_v.x; ob->vy = sm.obb.axis_v.y; ob->vz = sm.obb.axis_v.z; ob->hv = sm.obb.half_extent_v;
                ob->wx = sm.obb.axis_w.x; ob->wy = sm.obb.axis_w.y; ob->wz = sm.obb.axis_w.z; ob->hw = sm.obb.half_extent_w;
            }

            ++ci;
        }
    }

    // Write dynamic mesh entries
    const uint32_t static_vtx_count = vtx_heap.GetBufferCount();
    const uint32_t static_idx_count = idx_heap.GetBufferCount();
    const uint32_t fif = frame_index % 3;

    for (auto& e : dyn_ents) {
        const auto* gpu_info = mesh_mgr.GetMeshInfo(e.dyn_mesh->gpu_handle);
        if (!gpu_info) continue;

        const auto& vtx_alloc = gpu_info->streamed_vertex_alloc[fif];
        const auto& idx_alloc = gpu_info->streamed_index_alloc[fif];

        if (!vtx_alloc.IsValid() || !idx_alloc.IsValid()) continue;

        const uint32_t vertex_buf_slot = static_vtx_count + vtx_alloc.buffer_index;
        const uint32_t base_vertex = static_cast<uint32_t>(
            vtx_alloc.offset / sizeof(StandardMeshPipeline::Vertex));
        const uint32_t index_buf_slot = static_idx_count + idx_alloc.buffer_index;
        const uint32_t index_offset = static_cast<uint32_t>(
            idx_alloc.offset / sizeof(uint32_t));

        const auto pos = e.transform ? e.transform->position : glm::vec3(0);
        const auto scale = e.transform ? e.transform->scale : glm::vec3(1);
        const glm::quat rot = e.transform ? e.transform->rotation : glm::quat{1.0f, 0.0f, 0.0f, 0.0f};

        for (uint32_t s = 0; s < e.dyn_mesh->submesh_count; ++s) {
            const uint32_t si = e.dyn_mesh->first_submesh + s;
            const auto& sms = gpu_info->sub_meshes;
            const auto sm = (si < sms.size()) ? sms[si] : SubMesh{};

            if (auto* d = static_cast<DynamicEntry*>(frame_blocks.compact_dynamic->Get(ci))) {
                d->px = pos.x; d->py = pos.y; d->pz = pos.z; d->pad0 = 0;
                d->sx = scale.x; d->sy = scale.y; d->sz = scale.z; d->pad1 = 0;
                d->rx = rot.x; d->ry = rot.y; d->rz = rot.z; d->rw = rot.w;
            }

            if (auto* s2 = static_cast<StaticEntry*>(frame_blocks.compact_static->Get(ci))) {
                s2->index_start_packed = (index_buf_slot << 24) | (index_offset + sm.index_start);
                s2->index_range = sm.index_count;
                {
                    const auto& mat_def = MaterialManager::MaterialManager::Get().GetMaterial(sm.material_id);
                    s2->technique_texture =
                        (static_cast<uint32_t>(mat_def.texture_slot.value) << 16) |
                        mat_def.technique_id.value;
                }
                s2->vertex_info = (vertex_buf_slot << 24) | base_vertex;
            }

            if (auto* sp = static_cast<glm::vec4*>(frame_blocks.bounding_spheres->Get(ci))) {
                sp->x = sm.sphere.center.x;
                sp->y = sm.sphere.center.y;
                sp->z = sm.sphere.center.z;
                sp->w = sm.sphere.radius;
            }

            if (auto* ob = static_cast<OBBGPUEntry*>(frame_blocks.bounding_obb->Get(ci))) {
                ob->cx = sm.obb.center.x; ob->cy = sm.obb.center.y; ob->cz = sm.obb.center.z; ob->pad0 = 0;
                ob->ux = sm.obb.axis_u.x; ob->uy = sm.obb.axis_u.y; ob->uz = sm.obb.axis_u.z; ob->hu = sm.obb.half_extent_u;
                ob->vx = sm.obb.axis_v.x; ob->vy = sm.obb.axis_v.y; ob->vz = sm.obb.axis_v.z; ob->hv = sm.obb.half_extent_v;
                ob->wx = sm.obb.axis_w.x; ob->wy = sm.obb.axis_w.y; ob->wz = sm.obb.axis_w.z; ob->hw = sm.obb.half_extent_w;
            }

            ++ci;
        }
    }

    // Set entity count on renderer so subsequent passes know how many entries to process
    renderer.SetCurrentEntityCount(total_submeshes);

    // --- Phase 6: Update dynamic block descriptors for this frame ---
    const uint32_t dyn_vtx_block_count = mesh_mgr.GetDynamicVertexBlockCount(fif);
    const uint32_t dyn_idx_block_count = mesh_mgr.GetDynamicIndexBlockCount(fif);

    for (uint32_t bi = 0; bi < dyn_vtx_block_count; ++bi) {
        renderer.UpdateVertexBufferArrayElement(
            frame_index,
            static_vtx_count + bi,
            mesh_mgr.GetDynamicVertexBuffer(fif, bi),
            mesh_mgr.GetDynamicVertexBlockSize(fif));
    }
    for (uint32_t bi = 0; bi < dyn_idx_block_count; ++bi) {
        renderer.UpdateIndexBufferArrayElement(
            frame_index,
            static_idx_count + bi,
            mesh_mgr.GetDynamicIndexBuffer(fif, bi),
            mesh_mgr.GetDynamicIndexBlockSize(fif));
    }

    // --- Phase 7: Advance CPU-side lifecycle ---
    // NOTE: MeshManager::EndFrame is called in FrameRender (after rendering)
    // to ensure the GPU has finished consuming allocations before they are freed.
    mesh_registry.EndFrame(mesh_mgr, EVICTION_TIMEOUT_FRAMES);
}

}

