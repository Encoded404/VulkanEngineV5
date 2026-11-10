module;


export module VulkanEngine.MeshRenderSystem;

import std;

export import VulkanEngine.ECS.ComponentRegistry;
export import VulkanEngine.Components.Transform;
export import VulkanEngine.Components.MeshReference;
export import VulkanEngine.Components.DynamicMesh;
export import VulkanEngine.Components.MaterialOverride;
export import VulkanEngine.Components.TransformOverride;
export import VulkanEngine.Components.GeometryModifier;
export import VulkanEngine.Components.SkeletonPose;
export import VulkanEngine.MeshRegistry;
export import VulkanEngine.MeshManager;
export import VulkanEngine.SceneRenderer;
export import VulkanEngine.SceneLimits;
export import VulkanEngine.GpuResources.DeviceBufferHeap;
export import VulkanEngine.Mesh.MeshTypes;
export import VulkanEngine.MaterialManager;
export import VulkanEngine.TechniqueManager;
export import VulkanEngine.TechniqueManager.DefaultMeshTechnique;
export import VulkanEngine.BindlessManager;
export import VulkanEngine.ModifierManager;

export namespace VulkanEngine {

class MeshRenderSystem {
public:
    MeshRenderSystem() = default;

    MeshRenderSystem(const MeshRenderSystem&) = delete;
    MeshRenderSystem& operator=(const MeshRenderSystem&) = delete;

    static constexpr std::uint32_t EVICTION_TIMEOUT_FRAMES = 120;

    void ProcessFrame(ComponentRegistry& registry,
                      MeshRegistry& mesh_registry,
                      MeshManager& mesh_mgr,
                      SceneRenderer::SceneRenderer& renderer,
                      GpuResources::DeviceBufferHeap& vtx_heap,
                      GpuResources::DeviceBufferHeap& idx_heap,
                      MaterialManager::MaterialManager& material_mgr,
                      std::uint32_t frame_index);

private:
    struct DrawEntity {
        Entity* entity = nullptr;
        const Components::Transform* transform = nullptr;
        const Components::MeshReference* mesh_ref = nullptr;
    };

    struct DynamicEntity {
        Entity* entity = nullptr;
        const Components::Transform* transform = nullptr;
        const Components::DynamicMesh* dyn_mesh = nullptr;
    };

    // One runnable modifier stage. `instance` is null for the builtin skinning
    // entry, which the component never owns.
    struct ResolvedModifier {
        Modifiers::ModifierId id{};
        const Modifiers::ModifierDesc* desc = nullptr;
        const Components::ModifierInstance* instance = nullptr;
        std::uint32_t param_size = 0;
        std::uint32_t param_alignment = 16;
        std::uint32_t workgroup_size = 64;
        bool is_skinning = false;
    };

    // Everything the write pass needs for one submesh, resolved once.
    //
    // Gather used to resolve (and validate) each submesh while writing it, but the
    // palette and parameter buffers have to be sized from the whole frame's
    // totals before anything is written. So resolution runs first and produces
    // these plans; the write pass then only copies. `param_begin`/`palette_base`
    // are assigned in a second, allocation-only step because they depend on the
    // frame total that the resolution pass is still computing.
    struct SubmeshPlan {
        // DynamicEntry inputs: the composed entity/submesh placement.
        SubMeshTransform placement{};
        // Bounds, copied so the write pass needs no registry access.
        BoundingSphere sphere{};
        BoundingOBB obb{};
        // StaticEntry inputs.
        std::uint32_t index_start_packed = 0;
        std::uint32_t index_range = 0;
        std::uint32_t technique_material = 0;
        std::uint32_t vertex_info_packed = 0;
        std::uint32_t orm_packed = 0;
        std::uint32_t vertex_window_base = 0;
        std::uint32_t vertex_span = 0;
        std::uint32_t uv_slot = 0;
        std::int32_t uv_bias = 0;
        // Deform inputs: the source table slot and the absolute source element
        // index. Stage 0 of every stack reads the vertex table, skinned or not.
        std::uint32_t vertex_buf_slot = 0;
        std::uint32_t base_vertex = 0;
        // Modifier stack.
        std::uint32_t first_modifier = 0;
        std::uint32_t modifier_count = 0;
        std::uint32_t param_begin = 0;
        std::uint32_t param_bytes = 0;
        std::uint32_t param_alignment = 16;
        // Skin addressing.
        std::uint32_t skin_index = kNoSkin;
        std::uint32_t skin_slot = 0;
        std::int32_t skin_bias = 0;
        std::uint32_t extra_slot = 0;
        std::int32_t extra_bias = 0;
        std::uint32_t influence_sets = 0;
        std::uint32_t palette_base = 0;
        std::uint32_t palette_count = 0;
        const Components::SkeletonPose* pose = nullptr;
        // True for the first plan that allocated this palette; submeshes sharing a
        // skin share one copy, so only that plan copies the matrices.
        bool palette_first = false;
        bool skinned = false;
    };

    // Gather scratch, reused across frames. The vectors are cleared (not
    // freed) each frame, so their capacity persists at the historical peak and
    // a steady scene performs no allocation. Growing is amortized: the hint is
    // last frame's count, and the reserve target doubles the current capacity,
    // so a rising scene does not reserve once per frame. This replaces a
    // per-frame reserve of the full entity ceiling.
    template<typename T>
    static void EnsureGatherReserve(std::vector<T>& scratch, const std::size_t last_count) {
        const std::size_t hint =
            std::max<std::size_t>(last_count, SceneLimits::kInitialGatherCapacity);
        if (scratch.capacity() < hint) {
            scratch.reserve(std::max(hint, scratch.capacity() + scratch.capacity() / 2));
        }
    }

    std::vector<DrawEntity> static_ents_{};
    std::vector<DynamicEntity> dyn_ents_{};
    // Per-frame resolution output, reused across frames so a steady scene does no
    // allocation: `plans_` is one entry per written submesh and
    // `resolved_modifiers_` is the flat stage list they index into.
    std::vector<SubmeshPlan> plans_{};
    std::vector<ResolvedModifier> resolved_modifiers_{};
    std::vector<SceneRenderer::ModifierDispatch> dispatch_scratch_{};
    // (entity, skin) -> (palette_base, matrix count). Cleared, not freed, each
    // frame; submeshes of one object that share a skin share one palette copy.
    std::unordered_map<std::uint64_t, std::pair<std::uint32_t, std::uint32_t>> palette_cache_{};
    // Dynamic-mesh problems are per-handle, not per-frame: report each broken
    // handle once, and re-arm it when a later frame finds the mesh usable.
    std::unordered_set<std::uint32_t> reported_dynamic_mesh_issues_{};
    std::array<std::uint32_t, SceneRenderer::SceneRenderer::MAX_DRAW_GROUPS> tech_submeshes_{};
    std::size_t last_static_count_ = 0;
    std::size_t last_dyn_count_ = 0;
    std::size_t last_plan_count_ = 0;
    std::size_t last_modifier_count_ = 0;
    std::size_t last_dispatch_count_ = 0;
};

}

