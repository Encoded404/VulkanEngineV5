module;

#include <glm/glm.hpp> // NOLINT(misc-include-cleaner)
#include <glm/gtc/quaternion.hpp> // NOLINT(misc-include-cleaner)
#include <typeinfo> // NOLINT(misc-include-cleaner)

#include <logging/logging_macros.hpp>

module VulkanEngine.MeshRenderSystem;

import std;
import std.compat;

import logiface;

import vulkan;

import VulkanEngine.ECS.ComponentRegistry;
import VulkanEngine.Components.Transform;
import VulkanEngine.Components.MeshReference;
import VulkanEngine.Components.DynamicMesh;
import VulkanEngine.Components.OrmOverride;
import VulkanEngine.Components.MaterialOverride;
import VulkanEngine.Components.TransformOverride;
import VulkanEngine.Components.GeometryModifier;
import VulkanEngine.Components.SkeletonPose;
import VulkanEngine.MeshRegistry;
import VulkanEngine.MeshManager;
import VulkanEngine.SceneRenderer;
import VulkanEngine.GpuResources.DeviceBufferHeap;
import VulkanEngine.MaterialManager;
import VulkanEngine.MaterialManager.MaterialId;
import VulkanEngine.ModifierManager;
import VulkanEngine.ModifierId;
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

// The entity's Transform as the TRS value the per-submesh composition and the
// DynamicEntry mirror both use. An absent transform means identity placement.
SubMeshTransform ToSubMeshTransform(const Components::Transform* transform) {
    SubMeshTransform out{};
    if (transform == nullptr) return out;
    out.position = MeshVertexVec3{transform->position.x, transform->position.y, transform->position.z};
    out.rotation[0] = transform->rotation.x;
    out.rotation[1] = transform->rotation.y;
    out.rotation[2] = transform->rotation.z;
    out.rotation[3] = transform->rotation.w;
    out.scale = MeshVertexVec3{transform->scale.x, transform->scale.y, transform->scale.z};
    return out;
}

// Reports a transform-override discrepancy once per (cause, mesh, entity), the
// same once-per-problem shape the material override diagnostics use. The
// resolver stays pure; turning its status into a log is the gather's job.
void ReportTransformOverrideIssue(const Components::TransformOverrideStatus status,
                                  Components::TransformOverride* override_component,
                                  const std::uint32_t mesh_id,
                                  const std::uint64_t entity_id) {
    if (override_component == nullptr) return;
    switch (status) {
        case Components::TransformOverrideStatus::MeshMismatch:
            if (override_component->ShouldReport(OverrideSignature(3, mesh_id, entity_id))) {
                LOGIFACE_LOG(warn, "MeshRenderSystem: TransformOverride was authored for mesh " +
                             std::to_string(override_component->mesh_id) + " but is attached to mesh " +
                             std::to_string(mesh_id) + "; the override is ignored");
            }
            break;
        case Components::TransformOverrideStatus::RangeTooLarge:
            if (override_component->ShouldReport(OverrideSignature(4, mesh_id, entity_id))) {
                LOGIFACE_LOG(warn, "MeshRenderSystem: TransformOverride names more submeshes than mesh " +
                             std::to_string(mesh_id) + " has; the override is ignored");
            }
            break;
        default:
            break;
    }
}

// Material chosen for one object-local submesh, after applying (and validating)
// any per-object MaterialOverride. `technique` is the material's own technique
// and is never mixed with a different material's packing.
struct EffectiveMaterial {
    MaterialManager::MaterialId id{0};
    TechniqueManager::BaseTechnique* technique = nullptr;
    // Resolved draw key for the material (its technique + render-state group).
    // Written into the low bits of StaticEntry.technique_material so the GPU
    // buckets and pipelines by group, not by bare technique id.
    std::uint16_t group_id{0};
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
    EffectiveMaterial result{asset_material, nullptr, 0u, false};

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

    // A material's group is meaningful only when its technique resolved;
    // otherwise the gather writes the fallback base group.
    result.group_id = (result.technique != nullptr)
        ? material_mgr.GetGroupForMaterial(result.id)
        : 0u;

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
    static_ents_.clear();
    EnsureGatherReserve(static_ents_, last_static_count_);
    registry.ForEach<Components::Transform, Components::MeshReference>(
        [&](Entity& entity, Components::Transform& transform, Components::MeshReference& mesh_ref) {
            if (static_ents_.size() >= SceneLimits::kGatherEntityCap) return;
            static_ents_.push_back({ &entity, &transform, &mesh_ref });
        });

    // --- Phase 2: Collect dynamic mesh entities ---
    dyn_ents_.clear();
    EnsureGatherReserve(dyn_ents_, last_dyn_count_);
    registry.ForEach<Components::Transform, Components::DynamicMesh>(
        [&](Entity& entity, Components::Transform& transform, Components::DynamicMesh& dyn_mesh) {
            if (dyn_ents_.size() >= SceneLimits::kGatherEntityCap) return;
            dyn_ents_.push_back({ &entity, &transform, &dyn_mesh });
        });

    last_static_count_ = static_ents_.size();
    last_dyn_count_ = dyn_ents_.size();

    LOGIFACE_LOG(trace, "ProcessFrame: collected " + std::to_string(static_ents_.size()) +
                 " static and " + std::to_string(dyn_ents_.size()) + " dynamic entities");

    // --- Phase 3: Request GPU residency for static meshes ---
    for (auto& e : static_ents_) {
        const std::uint32_t mesh_id = e.mesh_ref->loaded_mesh_id;
        mesh_registry.RequestGpuResidency(mesh_id, mesh_mgr, renderer,
                                           vtx_heap, idx_heap);
    }

    // --- Phase 4: Count total submesh entries ---
    std::uint32_t total_submeshes = 0;

    for (auto& e : static_ents_) {
        const auto* info = mesh_registry.Get(e.mesh_ref->loaded_mesh_id);
        if (!info || !info->gpu_resident) continue;
        total_submeshes += info->submesh_count;
    }

    {
        const std::uint32_t dyn_fif = frame_index % mesh_mgr.GetFramesInFlight();
        for (auto& e : dyn_ents_) {
            const std::uint32_t handle_id = e.dyn_mesh->gpu_handle.id;
            const auto* gpu_info = mesh_mgr.GetMeshInfo(e.dyn_mesh->gpu_handle);
            if (!gpu_info) {
                if (LOGIFACE_ENABLED(debug) &&
                    reported_dynamic_mesh_issues_.insert(handle_id).second) {
                    LOGIFACE_LOG(debug, "ProcessFrame: dynamic mesh gpu_handle invalid (id=" +
                                 std::to_string(handle_id) +
                                 "), skipping until the handle is repaired");
                }
                continue;
            }
            if (!gpu_info->streamed_vertex_alloc[dyn_fif].IsValid() ||
                !gpu_info->streamed_index_alloc[dyn_fif].IsValid()) {
                if (LOGIFACE_ENABLED(debug) &&
                    reported_dynamic_mesh_issues_.insert(handle_id).second) {
                    LOGIFACE_LOG(debug, "ProcessFrame: dynamic mesh FIF " +
                                 std::to_string(dyn_fif) + " streamed alloc invalid (id=" +
                                 std::to_string(handle_id) +
                                 "), skipping until the allocation is repaired");
                }
                continue;
            }
            if (LOGIFACE_ENABLED(debug)) {
                reported_dynamic_mesh_issues_.erase(handle_id);
            }

            if (e.dyn_mesh->submesh_count > 0) {
                total_submeshes += e.dyn_mesh->submesh_count;
            } else {
                total_submeshes += 1;
            }
        }
    }

    // The descriptor binding arrays cap how many submeshes this frame can hold.
    // Clamp to the budget and truncate at a submesh boundary, deterministically,
    // instead of letting a block array grow past its binding array (which would
    // write descriptors out of range). Reported once per process. The clamp lands
    // before the plans are built so the count and write passes truncate at the
    // same boundary.
    const std::uint32_t max_submeshes = renderer.MaxSubmeshes();
    if (total_submeshes > max_submeshes) {
        static bool reported_overflow = false;
        if (!reported_overflow) {
            reported_overflow = true;
            LOGIFACE_LOG(error, "ProcessFrame: scene has " + std::to_string(total_submeshes) +
                " submeshes but the capacity budget is " + std::to_string(max_submeshes) +
                " (" + std::to_string(renderer.MaxBlocks()) + " blocks x " +
                std::to_string(SceneLimits::kEntriesPerBlock) +
                "); the frame is truncated");
        }
        total_submeshes = max_submeshes;
    }

    // Update streamed data for dynamic meshes
    for (auto& e : dyn_ents_) {
        mesh_mgr.UpdateStreamed(e.dyn_mesh->gpu_handle, e.dyn_mesh->mesh_data, frame_index);
    }

    // ── GPU mirror structs ──
    // These are byte-identical to the Slang structs (CDataLayout buffers,
    // scalar block layout device feature) in expand.slang. The static_asserts
    // turn any layout drift between CPU and GPU struct definitions into a
    // compile-time error instead of corrupted transforms.
    // No alignas on these: the engine's BlockArray packs entries back-to-back
    // at the exact stride, and scalar block layout requires no 16B struct
    // alignment on the GPU side. Natural alignment only.
    // Sentinel UV block slot: no out-of-line UV1 for this submesh. Mirrors
    // kNoUvSlot in scene_entries.slang.
    constexpr std::uint32_t kNoUvSlot = 0xFFFFFFFFu;

    struct DynamicEntry {
        float px, py, pz, pad0;
        float sx, sy, sz, pad1;
        float rx, ry, rz, rw;
    };
    static_assert(sizeof(DynamicEntry) == 48, "DynamicEntry must match Slang DynamicEntry (CDataLayout)");

    struct StaticEntry {
        std::uint32_t index_start_packed;
        std::uint32_t index_range;
        // Packed: hi 18 = material id, lo 14 = technique id
        // (TechniquePacking::TECHNIQUE_BITS = 14, BaseTechnique.cppm).
        std::uint32_t technique_material;
        std::uint32_t vertex_info_packed;
        std::uint32_t orm_packed;          // unorm8: [7:0]=AO, [15:8]=roughness, [23:16]=metallic, [31:24]=spare
        std::uint32_t vertex_window_base;  // min mesh-local index referenced by the submesh
        std::uint32_t vertex_span;         // number of distinct slots in the tight vertex window
        std::uint32_t uv_slot;             // out-of-line UV1 buffer block (kNoUvSlot when absent)
        std::int32_t uv_bias;              // uvRegionBase - meshVtxBase (signed re-base)
    };
    static_assert(sizeof(StaticEntry) == 36, "StaticEntry must match Slang StaticEntry (CDataLayout)");

    struct ObbEntry {
        float cx, cy, cz, pad0;
        float ux, uy, uz, hu;
        float vx, vy, vz, hv;
        float wx, wy, wz, hw;
    };
    static_assert(sizeof(ObbEntry) == 64, "ObbEntry must match Slang ObbEntry (CDataLayout)");

    // VertexEntry mirror (written by the GPU expand pass, only sized here).
    // Slang C layout: mvp@0 (64B) + maxScale@64 + materialId@68 + ormPacked@72
    // + modelMatrix@76 (64B) + normalMatrix@140 (3 tightly packed float3 rows,
    // 36B) + vertexBufferSlot@176 + uvSlot@180 + uvBias@184 = 188 bytes. No
    // alignment padding — matrices sit on 4-byte boundaries, which is exactly
    // what scalar block layout permits.
    struct VertexEntry {
        std::array<float, 16> mvp;           // 0
        float max_scale;                     // 64
        std::uint32_t material_id;           // 68
        std::uint32_t orm_packed;            // 72
        std::array<float, 16> model_matrix;  // 76
        std::array<float, 9> normal_matrix;  // 140 (row-major, 12B row stride)
        std::uint32_t vertex_buffer_slot;    // 176 (read by the MID vertex shaders)
        std::uint32_t uv_slot;               // 180 (out-of-line UV1 block; kNoUvSlot when absent)
        std::int32_t uv_bias;                // 184 (uvRegionBase - meshVtxBase)
    };
    static_assert(sizeof(VertexEntry) == 188, "VertexEntry must match Slang VertexEntry (CDataLayout)");

    // Pack ORM override factors into one u32: bits [7:0]=AO, [15:8]=roughness,
    // [23:16]=metallic, [31:24]=spare. Unorm8 per channel, must match the
    // unpacking in standard_mesh.slang.
    const auto PackOrm8 = [](float ao, float roughness, float metallic) -> std::uint32_t {
        const auto u8 = [](float v) -> std::uint32_t {
            return static_cast<std::uint32_t>(glm::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
        };
        return u8(ao) | (u8(roughness) << 8) | (u8(metallic) << 16);
    };

    // The 24-bit vertex index packing cannot be represented past 2^24; fail
    // loud (once) instead of silently corrupting geometry.
    const auto check_vertex_pack = [](std::uint32_t abs_vertex_end) {
        static bool reported = false;
        if (!reported && abs_vertex_end >= (1u << 24)) {
            reported = true;
            LOGIFACE_LOG(error, "vertex_info_packed 24-bit packing overflow: "
                "baseVertex + vertexWindowBase + vertexSpan >= 2^24; geometry will be corrupt");
        }
        assert(abs_vertex_end < (1u << 24) &&
               "vertex_info_packed 24-bit packing overflow");
    };

    // ── Resolution pass ──
    // One plan per written submesh, plus the flat stage list they index into. No
    // per-frame GPU buffer is touched here: the frame's totals have to be known
    // before the capacity-sized buffers (palettes, parameters) can be sized and
    // mapped, and a buffer pointer taken before that growth would dangle.
    resolved_modifiers_.clear();
    plans_.clear();
    palette_cache_.clear();
    EnsureGatherReserve(plans_, last_plan_count_);
    EnsureGatherReserve(resolved_modifiers_, last_modifier_count_);

    // Upper bound on the flat palette buffer. A malformed asset must not make the
    // engine reserve a mat4 per joint per object without limit; 2^18 matrices is
    // 16 MiB, far above any plausible frame.
    constexpr std::uint32_t kMaxPaletteMatrices = 1u << 18;
    std::uint32_t palette_cursor = 0;

    // Resolves the skin of one submesh: skin slot/bias, influence sets and the
    // palette reservation. Leaves `plan.skinned` false when any link is missing,
    // which renders the submesh through the plain vertex path.
    const auto resolve_skin = [&](SubmeshPlan& plan,
                                  const LoadedMeshInfo* const loaded,
                                  const Components::SkeletonPose* const pose,
                                  const std::uint32_t skin_index,
                                  const MeshManager::GpuMeshInfo* const gpu_info,
                                  const std::uint64_t entity_id) {
        plan.skin_index = skin_index;
        if (skin_index == kNoSkin || loaded == nullptr || pose == nullptr ||
            gpu_info == nullptr) {
            return;
        }
        if (skin_index >= loaded->cpu_data.skins.size()) {
            static bool reported = false;
            if (!reported) {
                reported = true;
                LOGIFACE_LOG(error, "ProcessFrame: submesh names skin " +
                    std::to_string(skin_index) + " but the mesh carries " +
                    std::to_string(loaded->cpu_data.skins.size()) + " skins; rendering bind pose");
            }
            return;
        }
        if (gpu_info->influence_sets == 0) return;

        const Skin& skin = loaded->cpu_data.skins[skin_index];
        if (skin.skeleton != pose->skeleton_index) return;
        const auto palette = pose->GetPalette(skin_index);
        if (palette.empty()) return;

        const auto count = static_cast<std::uint32_t>(palette.size());
        const std::uint64_t key = (entity_id << 32) | skin_index;
        const auto cached = palette_cache_.find(key);
        if (cached != palette_cache_.end()) {
            // Submeshes of one object that share a skin share one copy.
            plan.palette_base = cached->second.first;
            plan.palette_count = cached->second.second;
            plan.palette_first = false;
        } else {
            // Never clamp `palette_base`: a clamped base aliases another skin's
            // bones. Dropping the skin leaves the submesh in bind pose instead.
            // The budget is only charged for a new reservation, so a shared palette
            // is never rejected after it has been allocated.
            if (palette_cursor + count > kMaxPaletteMatrices) {
                static bool reported = false;
                if (!reported) {
                    reported = true;
                    LOGIFACE_LOG(error, "ProcessFrame: bone palettes exceed " +
                        std::to_string(kMaxPaletteMatrices) +
                        " matrices; skinned submeshes past the budget render bind pose");
                }
                return;
            }
            plan.palette_base = palette_cursor;
            plan.palette_count = count;
            plan.palette_first = true;
            palette_cache_.emplace(key, std::pair{palette_cursor, count});
            palette_cursor += count;
        }

        plan.pose = pose;
        plan.skin_slot = gpu_info->skin_buffer_index;
        plan.extra_slot = gpu_info->has_extra_skin ? gpu_info->skin_extra_buffer_index
                                                   : SceneRenderer::kNoSkinSlot;
        // The influence tables are their own element spaces, so the region base
        // re-bases the absolute source element index just like the UV path does.
        const std::uint32_t source_base = plan.base_vertex + plan.vertex_window_base;
        plan.skin_bias = SceneRenderer::DeformBias(gpu_info->skin_region_base, source_base);
        plan.extra_bias = SceneRenderer::DeformBias(gpu_info->skin_extra_region_base, source_base);
        plan.influence_sets = gpu_info->influence_sets;
        plan.skinned = true;
    };

    // Resolves one submesh's effective modifier stack: the builtin skinning stage
    // (when skinned and runnable) followed by the object stack and, for an
    // appending submesh, its own entries. Dead ids, parameter-size mismatches and
    // not-yet-built pipelines are dropped here, before stages are assigned, so a
    // drop can never leave a hole in the stage numbering.
    const auto resolve_stack = [&](SubmeshPlan& plan,
                                   Components::GeometryModifier* const gm,
                                   const std::uint32_t local_submesh,
                                   const std::uint32_t mesh_id,
                                   const std::uint32_t mesh_submesh_count,
                                   const std::uint64_t entity_id) {
        plan.first_modifier = static_cast<std::uint32_t>(resolved_modifiers_.size());
        plan.modifier_count = 0;
        plan.param_bytes = 0;
        plan.param_alignment = 16;

        if (plan.skinned) {
            const Modifiers::ModifierId skinning = renderer.SkinningModifier();
            const Modifiers::ModifierDesc* const desc =
                renderer.GetModifierManager().Get(skinning);
            if (skinning.IsValid() && desc != nullptr && renderer.IsModifierReady(skinning)) {
                resolved_modifiers_.push_back(ResolvedModifier{
                    .id = skinning,
                    .desc = desc,
                    .instance = nullptr,
                    .param_size = 0,
                    .param_alignment = 16,
                    .workgroup_size = desc->workgroup_size,
                    .is_skinning = true,
                });
                ++plan.modifier_count;
            } else {
                // No skinning pipeline: bind pose, and no stack whose first stage
                // does not exist. This is the state before the deform pass exists.
                plan.skinned = false;
            }
        }

        if (gm != nullptr) {
            if (gm->mesh_id == Components::GeometryModifier::kUnboundMesh) {
                gm->mesh_id = mesh_id;
            }
            if (gm->mesh_id != mesh_id) {
                const std::uint64_t sig = OverrideSignature(5, gm->mesh_id, mesh_id);
                if (gm->ShouldReport(sig)) {
                    LOGIFACE_LOG(warn, "GeometryModifier: entity " + std::to_string(entity_id) +
                        " stack was authored for mesh " + std::to_string(gm->mesh_id) +
                        " but entity now uses mesh " + std::to_string(mesh_id) +
                        "; the stack is ignored");
                }
                return;
            }
            if (gm->submeshes.size() > mesh_submesh_count) {
                const std::uint64_t sig = OverrideSignature(6, gm->submeshes.size(),
                                                            mesh_submesh_count);
                if (gm->ShouldReport(sig)) {
                    LOGIFACE_LOG(warn, "GeometryModifier: entity " + std::to_string(entity_id) +
                        " names " + std::to_string(gm->submeshes.size()) +
                        " submesh stacks but mesh " + std::to_string(mesh_id) + " has " +
                        std::to_string(mesh_submesh_count) +
                        "; entries past the end are ignored");
                }
            }

            Components::ForEachModifierInstance(
                *gm, local_submesh, [&](const Components::ModifierInstance& instance) {
                    const Modifiers::ModifierDesc* const desc =
                        renderer.GetModifierManager().Get(instance.id);
                    if (desc == nullptr) {
                        const std::uint64_t sig = OverrideSignature(
                            7, local_submesh, instance.id.value);
                        if (gm->ShouldReport(sig)) {
                            LOGIFACE_LOG(warn, "GeometryModifier: entity " +
                                std::to_string(entity_id) + " submesh " +
                                std::to_string(local_submesh) + " references modifier " +
                                std::to_string(instance.id.value) + " (generation " +
                                std::to_string(instance.id.generation) +
                                ") which is not registered; the stage is dropped");
                        }
                        return;
                    }
                    if (instance.params.size() != desc->param_size) {
                        const std::uint64_t sig = OverrideSignature(
                            8, instance.id.value, instance.params.size());
                        if (gm->ShouldReport(sig)) {
                            LOGIFACE_LOG(warn, "GeometryModifier: entity " +
                                std::to_string(entity_id) + " submesh " +
                                std::to_string(local_submesh) + " supplies " +
                                std::to_string(instance.params.size()) +
                                " parameter bytes to modifier " +
                                std::to_string(instance.id.value) + " which declares " +
                                std::to_string(desc->param_size) + "; the stage is dropped");
                        }
                        return;
                    }
                    // A modifier that has no pipeline yet is silently skipped: the
                    // first frames after registration look exactly like "no
                    // modifiers", which is the intended bind-pose fallback.
                    if (!renderer.IsModifierReady(instance.id)) return;
                    resolved_modifiers_.push_back(ResolvedModifier{
                        .id = instance.id,
                        .desc = desc,
                        .instance = &instance,
                        .param_size = desc->param_size,
                        .param_alignment = desc->param_alignment,
                        .workgroup_size = desc->workgroup_size,
                        .is_skinning = false,
                    });
                    ++plan.modifier_count;
                });
        }

        // Aligned parameter bytes for this submesh. A plan that can never run
        // (no vertices in its window) reserves nothing: `deform_active` requires a
        // non-zero span, so its blocks would be dead space in the buffer.
        if (plan.vertex_span == 0) return;
        std::uint32_t bytes = 0;
        std::uint32_t alignment = 16;
        for (std::uint32_t k = plan.first_modifier;
             k < plan.first_modifier + plan.modifier_count; ++k) {
            const ResolvedModifier& modifier = resolved_modifiers_[k];
            if (modifier.is_skinning || modifier.param_size == 0) continue;
            alignment = std::max(alignment, modifier.param_alignment);
            bytes = SceneRenderer::DeformParamOffset(bytes, modifier.param_alignment) +
                    modifier.param_size;
        }
        plan.param_bytes = bytes;
        plan.param_alignment = alignment;
    };

    std::uint32_t total_index_count = 0;
    std::uint32_t total_vertex_span = 0;
    constexpr std::uint32_t kTechniqueCount =
        SceneRenderer::SceneRenderer::MAX_DRAW_GROUPS;
    tech_submeshes_.fill(0);
    const auto accumulate = [&](const SubMesh& sm, std::uint32_t tech_material) {
        total_index_count += sm.index_count;
        total_vertex_span += sm.vertex_span;
        const std::uint32_t tid = tech_material & TechniqueManager::TechniquePacking::TECHNIQUE_MASK;
        if (tid < kTechniqueCount) tech_submeshes_[tid] += 1u;
    };

    auto& scene_submeshes = renderer.GetSubmeshes();

    // Write static mesh plans. The plan index is the global submesh slot; the
    // budget clamp above can stop the resolution part-way (truncation at a
    // submesh boundary).
    for (auto& e : static_ents_) {
        if (plans_.size() >= total_submeshes) break;
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

        const SubMeshTransform entity_transform = ToSubMeshTransform(e.transform);

        // Per-object ORM override: values multiply with the material's ORM texture.
        // The component applies to all submeshes of the entity; when absent, each
        // submesh falls back to its material's own factors (resolved per submesh).
        const Components::OrmOverride* orm_override = nullptr;
        Components::MaterialOverride* material_override = nullptr;
        Components::TransformOverride* transform_override = nullptr;
        Components::GeometryModifier* geometry_modifier = nullptr;
        const Components::SkeletonPose* pose = nullptr;
        if (e.entity != nullptr) {
            orm_override = e.entity->GetComponent<Components::OrmOverride>();
            material_override = e.entity->GetComponent<Components::MaterialOverride>();
            transform_override = e.entity->GetComponent<Components::TransformOverride>();
            geometry_modifier = e.entity->GetComponent<Components::GeometryModifier>();
            pose = e.entity->GetComponent<Components::SkeletonPose>();
        }
        const std::uint64_t entity_id = e.entity != nullptr
            ? static_cast<std::uint64_t>(e.entity->GetId().index)
            : 0ull;

        for (std::uint32_t s = 0; s < loaded->submesh_count; ++s) {
            if (plans_.size() >= total_submeshes) break;
            const std::uint32_t si = loaded->first_submesh_in_renderer + s;
            const auto sm = (si < scene_submeshes.size())
                ? scene_submeshes[si]
                : SubMesh{};

            SubmeshPlan plan{};
            plan.vertex_buf_slot = vertex_buf_slot;
            plan.base_vertex = base_vertex;
            plan.vertex_window_base = sm.vertex_window_base;
            plan.vertex_span = sm.vertex_span;
            plan.index_range = sm.index_count;
            plan.sphere = sm.sphere;
            plan.obb = sm.obb;
            plan.index_start_packed = SceneRenderer::PackEntry(index_buf_slot, sm.index_start);
            plan.vertex_info_packed = SceneRenderer::PackEntry(vertex_buf_slot, base_vertex);

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
            plan.orm_packed = PackOrm8(orm_ao, orm_roughness, orm_metallic);

            if (effective.technique != nullptr) {
                plan.technique_material =
                    effective.technique->PackMaterialData(effective.id.value, effective.group_id);
            } else {
                // Unreachable while the fallback material (id 0) is registered.
                assert(effective.technique != nullptr &&
                       "submesh has no resolvable technique; fallback material missing");
                plan.technique_material = TechniqueManager::TechniquePacking::Pack(0u, 0u);
            }

            // Out-of-line UV1: the mesh's UV region re-bases the absolute
            // vertex index. Absent -> sentinel, and the shader emits UV0.
            if (gpu_info->has_uv1) {
                plan.uv_slot = gpu_info->uv_buffer_index;
                plan.uv_bias = static_cast<std::int32_t>(gpu_info->uv_region_base) -
                               static_cast<std::int32_t>(base_vertex);
            } else {
                plan.uv_slot = kNoUvSlot;
                plan.uv_bias = 0;
            }
            check_vertex_pack(base_vertex + sm.vertex_window_base + sm.vertex_span);

            // The entity transform places the mesh; this submesh's own transform
            // places it inside the mesh (for glTF, the node's baked world
            // transform), unless a per-object override names it. Composing is
            // what keeps a flattened node hierarchy a single mesh without
            // duplicating geometry.
            const Components::EffectiveSubmeshTransform resolved =
                Components::ResolveEffectiveSubmeshTransform(
                    entity_transform, sm.transform, transform_override, s,
                    e.mesh_ref->loaded_mesh_id, loaded->submesh_count);
            ReportTransformOverrideIssue(resolved.status, transform_override,
                                         e.mesh_ref->loaded_mesh_id, entity_id);
            plan.placement = resolved.transform;

            resolve_skin(plan, loaded, pose, sm.skin_index, gpu_info, entity_id);
            resolve_stack(plan, geometry_modifier, s, e.mesh_ref->loaded_mesh_id,
                          loaded->submesh_count, entity_id);

            accumulate(sm, plan.technique_material);
            plans_.push_back(plan);
        }
    }

    // Write dynamic mesh plans. Streamed meshes carry no skin influences and no
    // modifier stack: their vertex and index streams are per-FIF copies that the
    // deform stage has no addressing for.
    const std::uint32_t static_vtx_count = vtx_heap.GetBufferCount();
    const std::uint32_t static_idx_count = idx_heap.GetBufferCount();
    const std::uint32_t fif = frame_index % mesh_mgr.GetFramesInFlight();

    for (auto& e : dyn_ents_) {
        if (plans_.size() >= total_submeshes) break;
        const auto* gpu_info = mesh_mgr.GetMeshInfo(e.dyn_mesh->gpu_handle);
        // The count pass above already reported an unusable handle or FIF
        // allocation once; the write pass stays quiet and just skips it.
        if (!gpu_info) continue;

        const auto& vtx_alloc = gpu_info->streamed_vertex_alloc[fif];
        const auto& idx_alloc = gpu_info->streamed_index_alloc[fif];

        if (!vtx_alloc.IsValid() || !idx_alloc.IsValid()) continue;

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
        const SubMeshTransform entity_transform = ToSubMeshTransform(e.transform);

        LOGIFACE_LOG(trace, "ProcessFrame: dynamic mesh transform pos=(" +
                     std::to_string(pos.x) + "," + std::to_string(pos.y) + "," + std::to_string(pos.z) +
                     ") scale=(" + std::to_string(scale.x) + "," + std::to_string(scale.y) + "," + std::to_string(scale.z) +
                     ") rot=(" + std::to_string(rot.x) + "," + std::to_string(rot.y) + "," + std::to_string(rot.z) + "," + std::to_string(rot.w) +
                     ")");

        // Per-object overrides for dynamic meshes. The gpu_handle acts as the
        // mesh fingerprint (dynamic meshes have no MeshRegistry id).
        const Components::OrmOverride* orm_override = nullptr;
        Components::MaterialOverride* material_override = nullptr;
        Components::TransformOverride* transform_override = nullptr;
        std::uint64_t entity_id = 0ull;
        if (e.entity != nullptr) {
            orm_override = e.entity->GetComponent<Components::OrmOverride>();
            material_override = e.entity->GetComponent<Components::MaterialOverride>();
            transform_override = e.entity->GetComponent<Components::TransformOverride>();
            entity_id = static_cast<std::uint64_t>(e.entity->GetId().index);
            if (e.entity->GetComponent<Components::GeometryModifier>() != nullptr) {
                static std::unordered_set<std::uint64_t> reported;
                if (reported.insert(entity_id).second) {
                    LOGIFACE_LOG(warn, "GeometryModifier: entity " + std::to_string(entity_id) +
                        " is a streamed dynamic mesh; modifier stacks are not supported on "
                        "streamed meshes and the component is ignored");
                }
            }
        }

        for (std::uint32_t s = 0; s < e.dyn_mesh->submesh_count; ++s) {
            if (plans_.size() >= total_submeshes) break;
            const std::uint32_t si = e.dyn_mesh->first_submesh + s;
            const auto& sms = gpu_info->sub_meshes;
            const auto sm = (si < sms.size()) ? sms[si] : SubMesh{};

            SubmeshPlan plan{};
            plan.vertex_buf_slot = vertex_buf_slot;
            plan.base_vertex = base_vertex;
            plan.vertex_window_base = sm.vertex_window_base;
            plan.vertex_span = sm.vertex_span;
            plan.index_range = sm.index_count;
            plan.sphere = sm.sphere;
            plan.obb = sm.obb;
            plan.index_start_packed = SceneRenderer::PackEntry(index_buf_slot,
                                                               index_offset + sm.index_start);
            plan.vertex_info_packed = SceneRenderer::PackEntry(vertex_buf_slot, base_vertex);
            // Streamed meshes have no out-of-line UV stream; they always sample UV0.
            plan.uv_slot = kNoUvSlot;
            plan.uv_bias = 0;

            const EffectiveMaterial effective = ResolveEffectiveMaterial(
                material_mgr, sm.material_id, material_override, s,
                e.dyn_mesh->gpu_handle.id, e.dyn_mesh->submesh_count, entity_id);

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
            plan.orm_packed = PackOrm8(orm_ao, orm_roughness, orm_metallic);

            if (effective.technique != nullptr) {
                plan.technique_material =
                    effective.technique->PackMaterialData(effective.id.value, effective.group_id);
            } else {
                // Unreachable while the fallback material (id 0) is registered.
                assert(effective.technique != nullptr &&
                       "dynamic submesh has no resolvable technique; fallback material missing");
                plan.technique_material = TechniqueManager::TechniquePacking::Pack(0u, 0u);
            }
            check_vertex_pack(base_vertex + sm.vertex_window_base + sm.vertex_span);

            const Components::EffectiveSubmeshTransform resolved =
                Components::ResolveEffectiveSubmeshTransform(
                    entity_transform, sm.transform, transform_override, s,
                    e.dyn_mesh->gpu_handle.id, e.dyn_mesh->submesh_count);
            ReportTransformOverrideIssue(resolved.status, transform_override,
                                         e.dyn_mesh->gpu_handle.id, entity_id);
            plan.placement = resolved.transform;

            accumulate(sm, plan.technique_material);
            plans_.push_back(plan);
        }
    }

    last_plan_count_ = plans_.size();
    last_modifier_count_ = resolved_modifiers_.size();

    // ── Frame totals ──
    // Deform regions are per submesh with a non-empty stack and a non-empty vertex
    // window; the deformed vertex buffer holds two regions (A and B) per such
    // submesh, so its span is one region, not two.
    std::uint32_t deform_vertex_span = 0;
    for (const SubmeshPlan& plan : plans_) {
        if (plan.modifier_count > 0 && plan.vertex_span > 0) {
            deform_vertex_span += plan.vertex_span;
        }
    }

    // Parameter blocks, in plan order. Each plan's block starts on its own maximum
    // alignment (at least 16 B) and its stages are then laid out contiguously.
    std::uint32_t param_cursor = 0;
    for (SubmeshPlan& plan : plans_) {
        plan.param_begin = param_cursor;
        if (plan.param_bytes == 0) continue;
        plan.param_begin = SceneRenderer::DeformParamOffset(param_cursor, plan.param_alignment);
        param_cursor = plan.param_begin + plan.param_bytes;
    }
    const std::uint32_t modifier_param_bytes = param_cursor;

    // Grow the indexed-drawing frame ring to the frame's real totals *before* any
    // buffer pointer is taken: EnsureSceneCapacity re-creates the capacity-sized
    // buffers (palettes, parameters) on growth, so a pointer mapped beforehand
    // would dangle.
    renderer.EnsureSceneCapacity(SceneRenderer::SceneCapacity{
        .index_count = total_index_count,
        .vertex_span = total_vertex_span,
        .submesh_count = total_submeshes,
        .deform_vertex_span = deform_vertex_span,
        .palette_matrix_count = palette_cursor,
        .modifier_param_bytes = modifier_param_bytes,
    });

    // --- Phase 5: Write per-frame data to renderer block arrays ---
    auto frame_blocks = renderer.GetFrameBlockArrays(frame_index);
    {
        const std::uint32_t blk_pre = frame_blocks.dynamic_entries->BlockCount();
        if (!frame_blocks.dynamic_entries->EnsureCapacity(total_submeshes)) {
            LOGIFACE_LOG(error, "ProcessFrame: dynamic_entries could not grow to " +
                         std::to_string(total_submeshes) + " entries");
        }
        LOGIFACE_LOG(trace, "ProcessFrame: dynamic_entries EnsureCapacity(" + std::to_string(total_submeshes) +
                     ") blocks " + std::to_string(blk_pre) + " -> " +
                     std::to_string(frame_blocks.dynamic_entries->BlockCount()));
    }
    {
        const std::uint32_t blk_pre = frame_blocks.static_entries->BlockCount();
        if (!frame_blocks.static_entries->EnsureCapacity(total_submeshes)) {
            LOGIFACE_LOG(error, "ProcessFrame: static_entries could not grow to " +
                         std::to_string(total_submeshes) + " entries");
        }
        LOGIFACE_LOG(trace, "ProcessFrame: static_entries EnsureCapacity(" + std::to_string(total_submeshes) +
                     ") blocks " + std::to_string(blk_pre) + " -> " +
                     std::to_string(frame_blocks.static_entries->BlockCount()));
    }
    if (!frame_blocks.bounding_spheres->EnsureCapacity(total_submeshes) ||
        !frame_blocks.obb_entries->EnsureCapacity(total_submeshes)) {
        LOGIFACE_LOG(error, "ProcessFrame: bounds block arrays could not grow to " +
                     std::to_string(total_submeshes) + " entries");
    }

    // The SkinEntry table is grown here, not by PrepareCompute: gather runs first
    // in the frame, and a block array whose Get() returns nullptr past BlockCount()
    // would silently drop every skinned submesh of the first frame.
    auto& skin_entries = renderer.GetFrameSkinEntries(frame_index);
    if (!skin_entries.EnsureCapacity(total_submeshes)) {
        LOGIFACE_LOG(error, "ProcessFrame: skin_entries could not grow to " +
                     std::to_string(total_submeshes) + " entries");
    }

    const auto palettes = renderer.MapFrameBonePalettes(frame_index);
    // Unmap in reverse order on every exit path: the buffers are host-visible, and
    // a second Map() without an Unmap() fails.
    struct FrameMapGuard {
        SceneRenderer::SceneRenderer* renderer = nullptr;
        std::uint32_t frame = 0;
        bool palettes_mapped = false;
        bool params_mapped = false;
        ~FrameMapGuard() {
            if (params_mapped) renderer->UnmapFrameModifierParams(frame);
            if (palettes_mapped) renderer->UnmapFrameBonePalettes(frame);
        }
    } map_guard{&renderer, frame_index, palettes.data() != nullptr, false};
    const auto params = renderer.MapFrameModifierParams(frame_index);
    map_guard.params_mapped = params.data() != nullptr;

    const bool palettes_ok = palettes.size() >= palette_cursor;
    const bool params_ok = modifier_param_bytes == 0 || params.size() >= modifier_param_bytes;
    bool dropped_any_stage = false;
    if (!palettes_ok && palette_cursor > 0) {
        static bool reported = false;
        if (!reported) {
            reported = true;
            LOGIFACE_LOG(error, "ProcessFrame: bone-palette buffer could not be mapped at its "
                "required size (" + std::to_string(palette_cursor) +
                " matrices); skinned submeshes render bind pose this frame");
        }
        dropped_any_stage = true;
    }
    if (!params_ok) {
        static bool reported = false;
        if (!reported) {
            reported = true;
            LOGIFACE_LOG(error, "ProcessFrame: modifier-parameter buffer could not be mapped at "
                "its required size (" + std::to_string(modifier_param_bytes) +
                " bytes); app modifiers are dropped this frame");
        }
        dropped_any_stage = true;
    }

    dispatch_scratch_.clear();
    EnsureGatherReserve(dispatch_scratch_, last_dispatch_count_);
    std::uint32_t written_param_end = 0;
    bool has_active_deform = false;

    for (std::size_t i = 0; i < plans_.size(); ++i) {
        const SubmeshPlan& plan = plans_[i];
        const auto index = static_cast<std::uint32_t>(i);

        if (auto* d = static_cast<DynamicEntry*>(frame_blocks.dynamic_entries->Get(index))) {
            d->px = plan.placement.position.x; d->py = plan.placement.position.y; d->pz = plan.placement.position.z; d->pad0 = 0;
            d->sx = plan.placement.scale.x; d->sy = plan.placement.scale.y; d->sz = plan.placement.scale.z; d->pad1 = 0;
            d->rx = plan.placement.rotation[0]; d->ry = plan.placement.rotation[1];
            d->rz = plan.placement.rotation[2]; d->rw = plan.placement.rotation[3];
        }

        if (auto* s2 = static_cast<StaticEntry*>(frame_blocks.static_entries->Get(index))) {
            s2->index_start_packed = plan.index_start_packed;
            s2->index_range = plan.index_range;
            s2->technique_material = plan.technique_material;
            s2->vertex_info_packed = plan.vertex_info_packed;
            s2->orm_packed = plan.orm_packed;
            s2->vertex_window_base = plan.vertex_window_base;
            s2->vertex_span = plan.vertex_span;
            s2->uv_slot = plan.uv_slot;
            s2->uv_bias = plan.uv_bias;
        }

        if (auto* sp = static_cast<glm::vec4*>(frame_blocks.bounding_spheres->Get(index))) {
            sp->x = plan.sphere.center.x;
            sp->y = plan.sphere.center.y;
            sp->z = plan.sphere.center.z;
            sp->w = plan.sphere.radius;
        }

        if (auto* ob = static_cast<ObbEntry*>(frame_blocks.obb_entries->Get(index))) {
            ob->cx = plan.obb.center.x; ob->cy = plan.obb.center.y; ob->cz = plan.obb.center.z; ob->pad0 = 0;
            ob->ux = plan.obb.axis_u.x; ob->uy = plan.obb.axis_u.y; ob->uz = plan.obb.axis_u.z; ob->hu = plan.obb.half_extent_u;
            ob->vx = plan.obb.axis_v.x; ob->vy = plan.obb.axis_v.y; ob->vz = plan.obb.axis_v.z; ob->hv = plan.obb.half_extent_v;
            ob->wx = plan.obb.axis_w.x; ob->wy = plan.obb.axis_w.y; ob->wz = plan.obb.axis_w.z; ob->hw = plan.obb.half_extent_w;
        }

        // Which stages actually run this frame. Palettes and parameter blocks are
        // capacity-sized, so a mapping that fell short drops those stages instead of
        // writing outside a buffer; a plan left with no stage renders through the
        // plain vertex path.
        const bool skinned = plan.skinned && palettes_ok;
        std::uint32_t stage_count = 0;
        for (std::uint32_t k = plan.first_modifier;
             k < plan.first_modifier + plan.modifier_count; ++k) {
            const ResolvedModifier& modifier = resolved_modifiers_[k];
            if (modifier.is_skinning ? skinned : params_ok) ++stage_count;
        }
        const bool deform_active = stage_count > 0 && plan.vertex_span > 0;
        // An even stack ends in region B: stage 0 writes A, stage 1 writes B, and
        // the final stage's region is what the vertex shader reads. Mirrors
        // SceneRenderer::DeformFinalRegion.
        const bool final_b = (stage_count % 2) == 0;

        // Copy the palette once per unique skin, on the plan that reserved it.
        if (skinned && plan.palette_first && plan.palette_count > 0 && plan.pose != nullptr) {
            const auto source = plan.pose->GetPalette(plan.skin_index);
            const std::size_t count =
                std::min<std::size_t>(source.size(), plan.palette_count);
            std::copy_n(source.begin(), count, palettes.begin() + plan.palette_base);
        }

        if (deform_active) {
            has_active_deform = true;
            std::uint32_t stage = 0;
            std::uint32_t cursor = plan.param_begin;
            for (std::uint32_t k = plan.first_modifier;
                 k < plan.first_modifier + plan.modifier_count; ++k) {
                const ResolvedModifier& modifier = resolved_modifiers_[k];
                if (modifier.is_skinning ? !skinned : !params_ok) continue;

                SceneRenderer::ModifierDispatch dispatch{};
                dispatch.submesh_index = index;
                dispatch.stage = stage++;
                dispatch.modifier = modifier.id;
                dispatch.vertex_count = plan.vertex_span;
                // Stage 0 always reads the vertex table; the skin tables are reached
                // through the SkinEntry, not through this slot.
                dispatch.src_slot = plan.vertex_buf_slot;
                dispatch.src_base = plan.base_vertex + plan.vertex_window_base;
                dispatch.workgroup_size = modifier.workgroup_size;
                if (!modifier.is_skinning) {
                    // A stage that declares no parameter block reserves nothing: it
                    // neither aligns the cursor nor advances it, so its
                    // param_offset is advisory. The count pass makes exactly the same
                    // choice, which is what keeps the two layouts identical.
                    dispatch.param_offset = cursor;
                    dispatch.param_size = modifier.param_size;
                    if (modifier.param_size > 0) {
                        cursor = SceneRenderer::DeformParamOffset(cursor,
                                                                  modifier.param_alignment);
                        dispatch.param_offset = cursor;
                        if (modifier.instance != nullptr) {
                            std::memcpy(params.data() + cursor,
                                        modifier.instance->params.data(),
                                        modifier.param_size);
                        }
                        cursor += modifier.param_size;
                    }
                }
                dispatch_scratch_.push_back(dispatch);
            }
            written_param_end = std::max(written_param_end, cursor);
        }

        if (auto* entry = static_cast<SceneRenderer::SkinEntry*>(skin_entries.Get(index))) {
            *entry = SceneRenderer::SkinEntry{};
            entry->extra_slot = SceneRenderer::kNoSkinSlot;
            if (skinned) {
                entry->palette_base = plan.palette_base;
                entry->skin_slot = plan.skin_slot;
                entry->skin_bias = plan.skin_bias;
                entry->extra_slot = plan.extra_slot;
                entry->extra_bias = plan.extra_bias;
                entry->influence_sets = plan.influence_sets;
            }
            if (deform_active) {
                // Bounds for a deformed object are no longer its rest-pose bounds,
                // so it must neither be culled nor selected as an occluder. This
                // covers unskinned modifier objects too: `bounds` may only narrow
                // it once an envelope exists.
                entry->flags = SceneRenderer::kDeformActive |
                               SceneRenderer::kDeformNoOccluder |
                               SceneRenderer::kDeformNoCullTest |
                               (skinned ? SceneRenderer::kSkinned : 0u) |
                               (final_b ? SceneRenderer::kFinalRegionB : 0u);
            } else {
                entry->flags = skinned ? SceneRenderer::kSkinned : 0u;
            }
            entry->deform_base = 0;
        }
    }

    last_dispatch_count_ = dispatch_scratch_.size();

    // The parameter layout is computed twice (count pass here, write pass above);
    // a disagreement means a dispatch would read another submesh's block.
    assert((dropped_any_stage || written_param_end == modifier_param_bytes) &&
           "gather count and write passes disagree on modifier parameter bytes");

    renderer.SetModifierDispatches(frame_index, dispatch_scratch_);
    renderer.SetHasActiveDeform(has_active_deform);
    renderer.SetTechniqueCommandRegions(tech_submeshes_);
    // Set entity count on renderer so subsequent passes know how many entries to process
    renderer.SetCurrentEntityCount(total_submeshes);

    // --- Phase 6: Update dynamic block descriptors for this frame ---
    const std::uint32_t dyn_vtx_block_count = mesh_mgr.GetDynamicVertexBlockCount(fif);
    const std::uint32_t dyn_idx_block_count = mesh_mgr.GetDynamicIndexBlockCount(fif);

    for (std::uint32_t bi = 0; bi < dyn_vtx_block_count; ++bi) {
        const vk::Buffer vbuf = mesh_mgr.GetDynamicVertexBuffer(fif, bi);
        LOGIFACE_LOG(trace, "ProcessFrame: update vertex_buffers_set frame=" +
                     std::to_string(frame_index) + " slot=" + std::to_string(static_vtx_count + bi));
        renderer.UpdateVertexBufferArrayElement(
            frame_index,
            static_vtx_count + bi,
            vbuf,
            mesh_mgr.GetDynamicVertexBlockSize(fif));
    }
    for (std::uint32_t bi = 0; bi < dyn_idx_block_count; ++bi) {
        const vk::Buffer ibuf = mesh_mgr.GetDynamicIndexBuffer(fif, bi);
        LOGIFACE_LOG(trace, "ProcessFrame: update index_buffers_set frame=" +
                     std::to_string(frame_index) + " slot=" + std::to_string(static_idx_count + bi));
        renderer.UpdateIndexBufferArrayElement(
            frame_index,
            static_idx_count + bi,
            ibuf,
            mesh_mgr.GetDynamicIndexBlockSize(fif));
    }

    LOGIFACE_LOG(trace, "ProcessFrame: fif=" + std::to_string(fif) +
                 " static_vtx=" + std::to_string(static_vtx_count) +
                 " static_idx=" + std::to_string(static_idx_count) +
                 " dyn_vtx_blocks=" + std::to_string(dyn_vtx_block_count) +
                 " dyn_idx_blocks=" + std::to_string(dyn_idx_block_count) +
                 " ents=" + std::to_string(total_submeshes) +
                 " plans=" + std::to_string(plans_.size()) +
                 " dispatches=" + std::to_string(dispatch_scratch_.size()));

    // --- Phase 7: Advance CPU-side lifecycle ---
    // NOTE: MeshManager::EndFrame is called in FrameRender (after rendering)
    // to ensure the GPU has finished consuming allocations before they are freed.
    mesh_registry.EndFrame(mesh_mgr, EVICTION_TIMEOUT_FRAMES);
}

}
