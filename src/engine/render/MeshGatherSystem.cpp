module;

#include <glm/glm.hpp> // NOLINT(misc-include-cleaner)
#include <glm/gtc/quaternion.hpp> // NOLINT(misc-include-cleaner)
#include <typeinfo> // NOLINT(misc-include-cleaner)

#include <logging/logging_macros.hpp>

module VulkanEngine.MeshRenderSystem;

import std;
import std.compat;

import logiface;

import vulkan_hpp;

import VulkanEngine.ECS.ComponentRegistry;
import VulkanEngine.Components.Transform;
import VulkanEngine.Components.MeshReference;
import VulkanEngine.Components.DynamicMesh;
import VulkanEngine.Components.OrmOverride;
import VulkanEngine.Components.MaterialOverride;
import VulkanEngine.MeshRegistry;
import VulkanEngine.MeshManager;
import VulkanEngine.SceneRenderer;
import VulkanEngine.GpuResources.DeviceBufferHeap;
import VulkanEngine.MaterialManager;
import VulkanEngine.MaterialManager.MaterialId;
import VulkanEngine.TechniqueManager;
import VulkanEngine.TechniqueManager.DefaultMeshTechnique;
import VulkanEngine.BindlessManager;

namespace VulkanEngine {

namespace {

// FNV-1a over a small tuple of integers. Used only to detect "same
// discrepancy as last frame" for once-per-cause logging.
std::uint64_t OverrideSignature(const std::uint64_t tag,
                                const std::uint64_t a,
                                const std::uint64_t b) {
    std::uint64_t h = 1469598103934665603ull;
    const auto mix = [&h](std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (i * 8)) & 0xFFu;
            h *= 1099511628211ull;
        }
    };
    mix(tag);
    mix(a);
    mix(b);
    return h;
}

// Material chosen for one object-local submesh, after applying (and validating)
// any per-object MaterialOverride. `technique` is the material's own technique
// and is never mixed with a different material's packing.
struct EffectiveMaterial {
    MaterialManager::MaterialId id{0};
    TechniqueManager::BaseTechnique* technique = nullptr;
    bool overridden = false;
};

// Resolve the material actually used for object-local submesh `submesh_index`.
//
// `asset_material` is the mesh asset's material for that submesh; `mesh_id` and
// `submesh_count` describe the owning mesh. Any inconsistency — stale mesh
// binding, an override range larger than the mesh, or an unusable (destroyed or
// reused) material reference — is reported once and falls back to the asset
// material rather than rendering with an unvalidated id.
//
// Never throws and never returns a packing for a material whose technique could
// not be resolved unless the asset material itself is broken (engine invariant).
EffectiveMaterial ResolveEffectiveMaterial(
    MaterialManager::MaterialManager& material_mgr,
    const MaterialManager::MaterialId asset_material,
    Components::MaterialOverride* const override_comp,
    const std::uint32_t submesh_index,
    const std::uint32_t mesh_id,
    const std::uint32_t submesh_count,
    const std::uint64_t entity_id) {
    EffectiveMaterial result{asset_material, nullptr, false};

    if (override_comp != nullptr) {
        // Lazily adopt the owner's mesh the first time the override is used.
        if (override_comp->mesh_id == Components::MaterialOverride::kUnboundMesh) {
            override_comp->mesh_id = mesh_id;
        }

        if (override_comp->mesh_id != mesh_id) {
            const std::uint64_t sig = OverrideSignature(1, override_comp->mesh_id, mesh_id);
            if (override_comp->ShouldReport(sig)) {
                LOGIFACE_LOG(warn, "MaterialOverride: entity " + std::to_string(entity_id) +
                    " override was authored for mesh " + std::to_string(override_comp->mesh_id) +
                    " but entity now uses mesh " + std::to_string(mesh_id) +
                    "; override ignored");
            }
        } else if (override_comp->submeshes.size() > submesh_count) {
            const std::uint64_t sig = OverrideSignature(2, override_comp->submeshes.size(),
                                                        submesh_count);
            if (override_comp->ShouldReport(sig)) {
                LOGIFACE_LOG(warn, "MaterialOverride: entity " + std::to_string(entity_id) +
                    " override covers " + std::to_string(override_comp->submeshes.size()) +
                    " submeshes but mesh " + std::to_string(mesh_id) + " has " +
                    std::to_string(submesh_count) + "; override ignored");
            }
        } else if (submesh_index < override_comp->submeshes.size()) {
            const MaterialManager::MaterialRef ref = override_comp->submeshes[submesh_index];
            if (ref.IsSet()) {
                if (!material_mgr.IsUsable(ref)) {
                    const std::uint64_t sig = OverrideSignature(3, ref.value, ref.generation);
                    if (override_comp->ShouldReport(sig)) {
                        LOGIFACE_LOG(warn, "MaterialOverride: entity " + std::to_string(entity_id) +
                            " submesh " + std::to_string(submesh_index) +
                            " references a destroyed or replaced material " +
                            std::to_string(ref.value) + " (generation " +
                            std::to_string(ref.generation) + "); using mesh material");
                    }
                } else {
                    result.id = MaterialManager::MaterialId{ref.value};
                    result.overridden = true;
                }
            }
        }
    }

    result.technique = material_mgr.GetTechniqueForMaterial(result.id);
    if (result.technique == nullptr && result.overridden) {
        const std::uint64_t sig = OverrideSignature(4, result.id.value, 0);
        if (override_comp->ShouldReport(sig)) {
            LOGIFACE_LOG(warn, "MaterialOverride: entity " + std::to_string(entity_id) +
                " override material " + std::to_string(result.id.value) +
                " has no registered technique; using mesh material");
        }
        result.id = asset_material;
        result.overridden = false;
        result.technique = material_mgr.GetTechniqueForMaterial(result.id);
    }

    return result;
}

// True when a technique's per-material data has the layout the ORM factor
// fallback reads. Guards against reinterpreting another technique's material
// bytes as DefaultMeshPerMaterialData.
bool TechniqueHasDefaultMeshMaterial(const TechniqueManager::BaseTechnique* const technique) {
    return technique != nullptr &&
           technique->HasPerMaterialOfType(
               std::type_index(typeid(TechniqueManager::DefaultMeshPerMaterialData)));
}

} // anonymous namespace

void MeshRenderSystem::ProcessFrame(ComponentRegistry& registry,
                                     MeshRegistry& mesh_registry,
                                     MeshManager& mesh_mgr,
                                     SceneRenderer::SceneRenderer& renderer,
                                     GpuResources::DeviceBufferHeap& vtx_heap,
                                     GpuResources::DeviceBufferHeap& idx_heap,
                                     MaterialManager::MaterialManager& material_mgr,
                                     std::uint32_t frame_index) {
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
            if (!owner) {
                LOGIFACE_LOG(debug, "ProcessFrame: DynamicMesh entity has no owner, skipping");
                return;
            }
            auto* transform = owner->GetComponent<Components::Transform>();
            if (!transform) {
                LOGIFACE_LOG(debug, "ProcessFrame: DynamicMesh entity has no Transform component, skipping");
                return;
            }
            dyn_ents.push_back({ transform, &dm });
        });

    LOGIFACE_LOG(trace, "ProcessFrame: collected " + std::to_string(static_ents.size()) +
                 " static and " + std::to_string(dyn_ents.size()) + " dynamic entities");

    // --- Phase 3: Request GPU residency for static meshes ---
    for (auto& e : static_ents) {
        const std::uint32_t mesh_id = e.mesh_ref->loaded_mesh_id;
        mesh_registry.RequestGpuResidency(mesh_id, mesh_mgr, renderer,
                                           vtx_heap, idx_heap);
    }

    // --- Phase 4: Count total submesh entries ---
    std::uint32_t total_submeshes = 0;

    for (auto& e : static_ents) {
        const auto* info = mesh_registry.Get(e.mesh_ref->loaded_mesh_id);
        if (!info || !info->gpu_resident) continue;
        total_submeshes += info->submesh_count;
    }

    {
        const std::uint32_t dyn_fif = frame_index % mesh_mgr.GetFramesInFlight();
        for (auto& e : dyn_ents) {
            const auto* gpu_info = mesh_mgr.GetMeshInfo(e.dyn_mesh->gpu_handle);
            if (!gpu_info) {
                LOGIFACE_LOG(debug, "ProcessFrame: dynamic mesh gpu_handle invalid, skipping in count");
                continue;
            }
            if (!gpu_info->streamed_vertex_alloc[dyn_fif].IsValid() ||
                !gpu_info->streamed_index_alloc[dyn_fif].IsValid()) {
                LOGIFACE_LOG(debug, "ProcessFrame: dynamic mesh FIF " + std::to_string(dyn_fif) +
                             " streamed alloc invalid, skipping in count");
                continue;
            }

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

    {
        const std::uint32_t blk_pre = frame_blocks.compact_dynamic->BlockCount();
        frame_blocks.compact_dynamic->EnsureCapacity(total_submeshes);
        LOGIFACE_LOG(trace, "ProcessFrame: compact_dynamic EnsureCapacity(" + std::to_string(total_submeshes) +
                     ") blocks " + std::to_string(blk_pre) + " -> " +
                     std::to_string(frame_blocks.compact_dynamic->BlockCount()));
    }
    {
        const std::uint32_t blk_pre = frame_blocks.compact_static->BlockCount();
        frame_blocks.compact_static->EnsureCapacity(total_submeshes);
        LOGIFACE_LOG(trace, "ProcessFrame: compact_static EnsureCapacity(" + std::to_string(total_submeshes) +
                     ") blocks " + std::to_string(blk_pre) + " -> " +
                     std::to_string(frame_blocks.compact_static->BlockCount()));
    }
    frame_blocks.bounding_spheres->EnsureCapacity(total_submeshes);
    frame_blocks.bounding_obb->EnsureCapacity(total_submeshes);

    // Pack ORM override factors into one u32: bits [7:0]=AO, [15:8]=roughness,
    // [23:16]=metallic, [31:24]=spare. Unorm8 per channel, must match the
    // unpacking in standard_mesh.slang.
    const auto PackOrm8 = [](float ao, float roughness, float metallic) -> std::uint32_t {
        const auto u8 = [](float v) -> std::uint32_t {
            return static_cast<std::uint32_t>(glm::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
        };
        return u8(ao) | (u8(roughness) << 8) | (u8(metallic) << 16);
    };

    // ── GPU mirror structs ──
    // These are byte-identical to the Slang structs (CDataLayout buffers,
    // scalar block layout device feature) in expand.slang. The static_asserts
    // turn any layout drift between CPU and GPU struct definitions into a
    // compile-time error instead of corrupted transforms.
    // No alignas on these: the engine's BlockArray packs entries back-to-back
    // at the exact stride, and scalar block layout requires no 16B struct
    // alignment on the GPU side. Natural alignment only.
    struct DynamicEntry {
        float px, py, pz, pad0;
        float sx, sy, sz, pad1;
        float rx, ry, rz, rw;
    };
    static_assert(sizeof(DynamicEntry) == 48, "DynamicEntry must match Slang DynEntry (CDataLayout)");

    struct StaticEntry {
        std::uint32_t index_start_packed;
        std::uint32_t index_range;
        std::uint32_t technique_material;  // packed: hi 16 = material_id, lo 16 = technique_id
        std::uint32_t vertex_info;
        std::uint32_t orm_packed;          // unorm8: [7:0]=AO, [15:8]=roughness, [23:16]=metallic, [31:24]=spare
        std::uint32_t vertex_window_base;  // min mesh-local index referenced by the submesh
        std::uint32_t vertex_span;         // number of distinct slots in the tight vertex window
    };
    static_assert(sizeof(StaticEntry) == 28, "StaticEntry must match Slang StaticEntry (CDataLayout)");

    struct OBBGPUEntry {
        float cx, cy, cz, pad0;
        float ux, uy, uz, hu;
        float vx, vy, vz, hv;
        float wx, wy, wz, hw;
    };
    static_assert(sizeof(OBBGPUEntry) == 64, "OBBGPUEntry must match Slang OBBGPU (CDataLayout)");

    // VertEntry mirror (written by the GPU expand pass, only sized here).
    // Slang C layout: MVP@0 (64B) + maxScale@64 + materialId@68 + ormPacked@72
    // + modelMatrix@76 (64B) + normalMatrix@140 (3 tightly packed float3 rows,
    // 36B) = 176 bytes. No alignment padding — matrices sit on 4-byte
    // boundaries, which is exactly what scalar block layout permits.
    struct VertEntryGPU {
        std::array<float, 16> mvp;           // 0
        float max_scale;                     // 64
        std::uint32_t material_id;           // 68
        std::uint32_t orm_packed;            // 72
        std::array<float, 16> model_matrix;  // 76
        std::array<float, 9> normal_matrix;  // 140 (row-major, 12B row stride)
    };
    static_assert(sizeof(VertEntryGPU) == 176, "VertEntryGPU must match Slang VertEntry (CDataLayout)");

    std::uint32_t ci = 0;

    // Indexed-drawing capacity totals + MID per-technique command regions.
    std::uint32_t total_index_count = 0;
    std::uint32_t total_vertex_span = 0;
    constexpr std::uint32_t kTechniqueCount =
        SceneRenderer::SceneRenderer::MAX_TECHNIQUES;
    std::vector<std::uint32_t> tech_submeshes(kTechniqueCount, 0);
    const auto accumulate = [&](const SubMesh& sm, std::uint32_t tech_material) {
        total_index_count += sm.index_count;
        total_vertex_span += sm.vertex_span;
        const std::uint32_t tid = tech_material & TechniqueManager::TechniquePacking::TECHNIQUE_MASK;
        if (tid < kTechniqueCount) tech_submeshes[tid] += 1u;
    };
    // The 24-bit vertex index packing cannot be represented past 2^24; fail
    // loud (once) instead of silently corrupting geometry.
    const auto check_vertex_pack = [](std::uint32_t abs_vertex_end) {
        static bool reported = false;
        if (!reported && abs_vertex_end >= (1u << 24)) {
            reported = true;
            LOGIFACE_LOG(error, "vertex_info 24-bit packing overflow: "
                "baseVertex + vertexWindowBase + vertexSpan >= 2^24; geometry will be corrupt");
        }
        assert(abs_vertex_end < (1u << 24) &&
               "vertex_info 24-bit packing overflow");
    };

    // Write static mesh entries
    for (auto& e : static_ents) {
        const auto* loaded = mesh_registry.Get(e.mesh_ref->loaded_mesh_id);
        if (!loaded || !loaded->gpu_resident || !loaded->gpu_handle.IsValid()) continue;

        const auto* gpu_info = mesh_mgr.GetMeshInfo(loaded->gpu_handle);
        if (!gpu_info) continue;

        const std::uint32_t vertex_buf_slot = gpu_info->vertex_allocation.buffer_index;
        // baseVertex must reconstruct the exact byte offset: integer division
        // here truncates unless the allocation was stride-aligned (see
        // kVertexAlignment in MeshUploadManager). Verify, don't hope.
        assert(gpu_info->vertex_allocation.offset % sizeof(StandardMeshPipeline::Vertex) == 0 &&
               "vertex allocation not stride-aligned; baseVertex would truncate");
        const std::uint32_t base_vertex = static_cast<std::uint32_t>(
            gpu_info->vertex_allocation.offset / sizeof(StandardMeshPipeline::Vertex));
        const std::uint32_t index_buf_slot = gpu_info->index_allocation.buffer_index;

        const auto pos = e.transform ? e.transform->position : glm::vec3(0);
        const auto scale = e.transform ? e.transform->scale : glm::vec3(1);
        const glm::quat rot = e.transform ? e.transform->rotation : glm::quat{1.0f, 0.0f, 0.0f, 0.0f};

        // Per-object ORM override: values multiply with the material's ORM texture.
        // The component applies to all submeshes of the entity; when absent, each
        // submesh falls back to its material's own factors (resolved per submesh).
        const Components::OrmOverride* orm_override = nullptr;
        Components::MaterialOverride* material_override = nullptr;
        if (e.mesh_ref && e.mesh_ref->GetOwner()) {
            orm_override = e.mesh_ref->GetOwner()->GetComponent<Components::OrmOverride>();
            material_override = e.mesh_ref->GetOwner()->GetComponent<Components::MaterialOverride>();
        }
        const std::uint64_t entity_id = (e.mesh_ref && e.mesh_ref->GetOwner())
            ? static_cast<std::uint64_t>(e.mesh_ref->GetOwner()->GetId())
            : 0ull;

        for (std::uint32_t s = 0; s < loaded->submesh_count; ++s) {
            const std::uint32_t si = loaded->first_submesh_in_renderer + s;
            const auto sm = (si < scene_submeshes.size())
                ? scene_submeshes[si]
                : SubMesh{};

            // Per-submesh material selection. The chosen material's own technique
            // travels with it, so a cross-technique override stays consistent.
            const EffectiveMaterial effective = ResolveEffectiveMaterial(
                material_mgr, sm.material_id, material_override, s,
                e.mesh_ref->loaded_mesh_id, loaded->submesh_count, entity_id);

            float orm_ao = 1.0f;
            float orm_roughness = 1.0f;
            float orm_metallic = 1.0f;
            if (orm_override) {
                orm_ao = orm_override->ao;
                orm_roughness = orm_override->roughness;
                orm_metallic = orm_override->metallic;
            } else if (TechniqueHasDefaultMeshMaterial(effective.technique) &&
                       material_mgr.IsUsable(effective.id)) {
                const auto& mat = material_mgr.Get<TechniqueManager::DefaultMeshPerMaterialData>(
                    effective.id);
                orm_ao = mat.ao_factor;
                orm_roughness = mat.roughness_factor;
                orm_metallic = mat.metallic_factor;
            }

            if (auto* d = static_cast<DynamicEntry*>(frame_blocks.compact_dynamic->Get(ci))) {
                d->px = pos.x; d->py = pos.y; d->pz = pos.z; d->pad0 = 0;
                d->sx = scale.x; d->sy = scale.y; d->sz = scale.z; d->pad1 = 0;
                d->rx = rot.x; d->ry = rot.y; d->rz = rot.z; d->rw = rot.w;
            }

            if (auto* s2 = static_cast<StaticEntry*>(frame_blocks.compact_static->Get(ci))) {
                s2->index_start_packed = (index_buf_slot << 24) | sm.index_start;
                s2->index_range = sm.index_count;
                if (effective.technique != nullptr) {
                    s2->technique_material = effective.technique->PackMaterialData(effective.id.value);
                } else {
                    // Unreachable while the fallback material (id 0) is registered.
                    assert(effective.technique != nullptr &&
                           "submesh has no resolvable technique; fallback material missing");
                    s2->technique_material =
                        TechniqueManager::TechniquePacking::Pack(0u, 0u);
                }
                s2->vertex_info = (vertex_buf_slot << 24) | base_vertex;
                s2->orm_packed = PackOrm8(orm_ao, orm_roughness, orm_metallic);
                s2->vertex_window_base = sm.vertex_window_base;
                s2->vertex_span = sm.vertex_span;
                check_vertex_pack(base_vertex + sm.vertex_window_base + sm.vertex_span);
                accumulate(sm, s2->technique_material);
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
    const std::uint32_t static_vtx_count = vtx_heap.GetBufferCount();
    const std::uint32_t static_idx_count = idx_heap.GetBufferCount();
    const std::uint32_t fif = frame_index % mesh_mgr.GetFramesInFlight();

    for (auto& e : dyn_ents) {
        const auto* gpu_info = mesh_mgr.GetMeshInfo(e.dyn_mesh->gpu_handle);
        if (!gpu_info) {
            LOGIFACE_LOG(debug, "ProcessFrame: dynamic mesh gpu_handle invalid, skipping write");
            continue;
        }

        const auto& vtx_alloc = gpu_info->streamed_vertex_alloc[fif];
        const auto& idx_alloc = gpu_info->streamed_index_alloc[fif];

        if (!vtx_alloc.IsValid() || !idx_alloc.IsValid()) {
            LOGIFACE_LOG(debug, "ProcessFrame: dynamic mesh FIF " + std::to_string(fif) +
                         " streamed alloc invalid, skipping write");
            continue;
        }

        const std::uint32_t vertex_buf_slot = static_vtx_count + vtx_alloc.buffer_index;
        assert(vtx_alloc.offset % sizeof(StandardMeshPipeline::Vertex) == 0 &&
               "streamed vertex allocation not stride-aligned; baseVertex would truncate");
        const std::uint32_t base_vertex = static_cast<std::uint32_t>(
            vtx_alloc.offset / sizeof(StandardMeshPipeline::Vertex));
        const std::uint32_t index_buf_slot = static_idx_count + idx_alloc.buffer_index;
        const std::uint32_t index_offset = static_cast<std::uint32_t>(
            idx_alloc.offset / sizeof(std::uint32_t));

        const auto pos = e.transform ? e.transform->position : glm::vec3(0);
        const auto scale = e.transform ? e.transform->scale : glm::vec3(1);
        const glm::quat rot = e.transform ? e.transform->rotation : glm::quat{1.0f, 0.0f, 0.0f, 0.0f};

        LOGIFACE_LOG(trace, "ProcessFrame: dynamic mesh transform pos=(" +
                     std::to_string(pos.x) + "," + std::to_string(pos.y) + "," + std::to_string(pos.z) +
                     ") scale=(" + std::to_string(scale.x) + "," + std::to_string(scale.y) + "," + std::to_string(scale.z) +
                     ") rot=(" + std::to_string(rot.x) + "," + std::to_string(rot.y) + "," + std::to_string(rot.z) + "," + std::to_string(rot.w) +
                     ")");

        // Per-object overrides for dynamic meshes. The gpu_handle acts as the
        // mesh fingerprint (dynamic meshes have no MeshRegistry id).
        const Components::OrmOverride* orm_override = nullptr;
        Components::MaterialOverride* material_override = nullptr;
        std::uint64_t entity_id = 0ull;
        if (e.dyn_mesh->GetOwner() != nullptr) {
            orm_override = e.dyn_mesh->GetOwner()->GetComponent<Components::OrmOverride>();
            material_override = e.dyn_mesh->GetOwner()->GetComponent<Components::MaterialOverride>();
            entity_id = static_cast<std::uint64_t>(e.dyn_mesh->GetOwner()->GetId());
        }

        for (std::uint32_t s = 0; s < e.dyn_mesh->submesh_count; ++s) {
            const std::uint32_t si = e.dyn_mesh->first_submesh + s;
            const auto& sms = gpu_info->sub_meshes;
            const auto sm = (si < sms.size()) ? sms[si] : SubMesh{};

            const EffectiveMaterial effective = ResolveEffectiveMaterial(
                material_mgr, sm.material_id, material_override, s,
                e.dyn_mesh->gpu_handle.id, e.dyn_mesh->submesh_count, entity_id);

            {
                auto* d = static_cast<DynamicEntry*>(frame_blocks.compact_dynamic->Get(ci));
                if (d) {
                    LOGIFACE_LOG(trace, "ProcessFrame: writing compact_dynamic[" + std::to_string(ci) +
                                 "] pos=(" + std::to_string(pos.x) + "," + std::to_string(pos.y) + "," + std::to_string(pos.z) +
                                 ") scale=(" + std::to_string(scale.x) + "," + std::to_string(scale.y) + "," + std::to_string(scale.z) +
                                 ") rot=(" + std::to_string(rot.x) + "," + std::to_string(rot.y) + "," + std::to_string(rot.z) + "," + std::to_string(rot.w) +
                                 ")");
                    d->px = pos.x; d->py = pos.y; d->pz = pos.z; d->pad0 = 0;
                    d->sx = scale.x; d->sy = scale.y; d->sz = scale.z; d->pad1 = 0;
                    d->rx = rot.x; d->ry = rot.y; d->rz = rot.z; d->rw = rot.w;
                    LOGIFACE_LOG(trace, "ProcessFrame: written compact_dynamic[" + std::to_string(ci) +
                                 "] verify px=" + std::to_string(d->px) + " py=" + std::to_string(d->py) +
                                 " sx=" + std::to_string(d->sx) + " rx=" + std::to_string(d->rx));
                } else {
                    LOGIFACE_LOG(warn, "ProcessFrame: compact_dynamic->Get(" + std::to_string(ci) + ") returned null");
                }
            }

            {
                auto* s2 = static_cast<StaticEntry*>(frame_blocks.compact_static->Get(ci));
                if (s2) {
                    const std::uint32_t packed_index = (index_buf_slot << 24) | (index_offset + sm.index_start);
                    const std::uint32_t packed_vertex = (vertex_buf_slot << 24) | base_vertex;
                    LOGIFACE_LOG(trace, "ProcessFrame: writing compact_static[" + std::to_string(ci) +
                                 "] index_buf_slot=" + std::to_string(index_buf_slot) +
                                 " index_offset=" + std::to_string(index_offset) +
                                 " sm.index_start=" + std::to_string(sm.index_start) +
                                 " packed_index=0x" + std::to_string(packed_index) +
                                 " vertex_buf_slot=" + std::to_string(vertex_buf_slot) +
                                 " base_vertex=" + std::to_string(base_vertex) +
                                 " packed_vertex=0x" + std::to_string(packed_vertex));
                    s2->index_start_packed = packed_index;
                    s2->index_range = sm.index_count;
                    if (effective.technique != nullptr) {
                        s2->technique_material =
                            effective.technique->PackMaterialData(effective.id.value);
                    } else {
                        // Unreachable while the fallback material (id 0) is registered.
                        assert(effective.technique != nullptr &&
                               "dynamic submesh has no resolvable technique; fallback material missing");
                        s2->technique_material =
                            TechniqueManager::TechniquePacking::Pack(0u, 0u);
                    }

                    // Same ORM precedence as static meshes: object override, then
                    // the effective material's factors.
                    float orm_ao = 1.0f;
                    float orm_roughness = 1.0f;
                    float orm_metallic = 1.0f;
                    if (orm_override) {
                        orm_ao = orm_override->ao;
                        orm_roughness = orm_override->roughness;
                        orm_metallic = orm_override->metallic;
                    } else if (TechniqueHasDefaultMeshMaterial(effective.technique) &&
                               material_mgr.IsUsable(effective.id)) {
                        const auto& mat = material_mgr.Get<TechniqueManager::DefaultMeshPerMaterialData>(
                            effective.id);
                        orm_ao = mat.ao_factor;
                        orm_roughness = mat.roughness_factor;
                        orm_metallic = mat.metallic_factor;
                    }
                    s2->orm_packed = PackOrm8(orm_ao, orm_roughness, orm_metallic);
                    s2->vertex_info = packed_vertex;
                    s2->vertex_window_base = sm.vertex_window_base;
                    s2->vertex_span = sm.vertex_span;
                    check_vertex_pack(base_vertex + sm.vertex_window_base + sm.vertex_span);
                    accumulate(sm, s2->technique_material);
                } else {
                    LOGIFACE_LOG(warn, "ProcessFrame: compact_static->Get(" + std::to_string(ci) + ") returned null");
                }
            }

            {
                auto* sp = static_cast<glm::vec4*>(frame_blocks.bounding_spheres->Get(ci));
                if (sp) {
                    sp->x = sm.sphere.center.x;
                    sp->y = sm.sphere.center.y;
                    sp->z = sm.sphere.center.z;
                    sp->w = sm.sphere.radius;
                }
            }

            {
                auto* ob = static_cast<OBBGPUEntry*>(frame_blocks.bounding_obb->Get(ci));
                if (ob) {
                    ob->cx = sm.obb.center.x; ob->cy = sm.obb.center.y; ob->cz = sm.obb.center.z; ob->pad0 = 0;
                    ob->ux = sm.obb.axis_u.x; ob->uy = sm.obb.axis_u.y; ob->uz = sm.obb.axis_u.z; ob->hu = sm.obb.half_extent_u;
                    ob->vx = sm.obb.axis_v.x; ob->vy = sm.obb.axis_v.y; ob->vz = sm.obb.axis_v.z; ob->hv = sm.obb.half_extent_v;
                    ob->wx = sm.obb.axis_w.x; ob->wy = sm.obb.axis_w.y; ob->wz = sm.obb.axis_w.z; ob->hw = sm.obb.half_extent_w;
                }
            }

            ++ci;
        }
    }

    // Grow the indexed-drawing frame ring to the frame's real totals before
    // any descriptor is written for this frame (PrepareCompute runs later).
    renderer.EnsureSceneCapacity(SceneRenderer::SceneCapacity{
        .index_count = total_index_count,
        .vertex_span = total_vertex_span,
        .submesh_count = total_submeshes,
    });
    renderer.SetTechniqueCommandRegions(tech_submeshes);

    // Set entity count on renderer so subsequent passes know how many entries to process
    renderer.SetCurrentEntityCount(total_submeshes);

    // --- Phase 6: Update dynamic block descriptors for this frame ---
    const std::uint32_t dyn_vtx_block_count = mesh_mgr.GetDynamicVertexBlockCount(fif);
    const std::uint32_t dyn_idx_block_count = mesh_mgr.GetDynamicIndexBlockCount(fif);

    for (std::uint32_t bi = 0; bi < dyn_vtx_block_count; ++bi) {
        const vk::Buffer vbuf = mesh_mgr.GetDynamicVertexBuffer(fif, bi);
        LOGIFACE_LOG(trace, "ProcessFrame: update bindless_vertex_set frame=" +
                     std::to_string(frame_index) + " slot=" + std::to_string(static_vtx_count + bi));
        renderer.UpdateVertexBufferArrayElement(
            frame_index,
            static_vtx_count + bi,
            vbuf,
            mesh_mgr.GetDynamicVertexBlockSize(fif));
    }
    for (std::uint32_t bi = 0; bi < dyn_idx_block_count; ++bi) {
        const vk::Buffer ibuf = mesh_mgr.GetDynamicIndexBuffer(fif, bi);
        LOGIFACE_LOG(trace, "ProcessFrame: update bindless_index_set frame=" +
                     std::to_string(frame_index) + " slot=" + std::to_string(static_idx_count + bi));
        renderer.UpdateIndexBufferArrayElement(
            frame_index,
            static_idx_count + bi,
            ibuf,
            mesh_mgr.GetDynamicIndexBlockSize(fif));
    }

    // Verify compact_dynamic content from mapped memory
    if (ci > 0) {
        for (std::uint32_t vi = 0; vi < std::min(ci, 4u); ++vi) {
            auto* d = static_cast<const DynamicEntry*>(frame_blocks.compact_dynamic->Get(vi));
            if (d) {
                LOGIFACE_LOG(trace, "ProcessFrame: compact_dynamic verify[" + std::to_string(vi) +
                             "] px=" + std::to_string(d->px) + " py=" + std::to_string(d->py) + " pz=" + std::to_string(d->pz) +
                             " sx=" + std::to_string(d->sx) + " sy=" + std::to_string(d->sy) + " sz=" + std::to_string(d->sz) +
                             " rx=" + std::to_string(d->rx) + " ry=" + std::to_string(d->ry) + " rz=" + std::to_string(d->rz) + " rw=" + std::to_string(d->rw));
            }
        }
    }

    LOGIFACE_LOG(trace, "ProcessFrame: fif=" + std::to_string(fif) +
                 " static_vtx=" + std::to_string(static_vtx_count) +
                 " static_idx=" + std::to_string(static_idx_count) +
                 " dyn_vtx_blocks=" + std::to_string(dyn_vtx_block_count) +
                 " dyn_idx_blocks=" + std::to_string(dyn_idx_block_count) +
                 " ents=" + std::to_string(total_submeshes) +
                 " ci=" + std::to_string(ci));

    // --- Phase 7: Advance CPU-side lifecycle ---
    // NOTE: MeshManager::EndFrame is called in FrameRender (after rendering)
    // to ensure the GPU has finished consuming allocations before they are freed.
    mesh_registry.EndFrame(mesh_mgr, EVICTION_TIMEOUT_FRAMES);
}

}

