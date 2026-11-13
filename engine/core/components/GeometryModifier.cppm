module;

export module VulkanEngine.Components.GeometryModifier;

import std;

import VulkanEngine.ECS.ComponentRegistry;
export import VulkanEngine.ModifierId;

export namespace VulkanEngine::Components {

// One modifier invocation in a stack: which registered kernel to run and the
// bytes it reads through the flat parameter buffer. `params.size()` must equal
// the ModifierDesc's `param_size`; a mismatch drops the instance at gather time
// rather than feeding a shader a short block.
struct ModifierInstance {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    Modifiers::ModifierId id{};
    std::vector<std::byte> params;
    // NOLINTEND(misc-non-private-member-variables-in-classes)
};

enum class ModifierStackMode : std::uint8_t {
    Append,  // object stack first, then this submesh's entries
    Replace, // this submesh's entries only (empty = no modifiers)
};

// Per-object-local-submesh override of the object's modifier stack.
struct SubMeshModifierStack {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    ModifierStackMode mode = ModifierStackMode::Append;
    std::vector<ModifierInstance> modifiers;
    // NOLINTEND(misc-non-private-member-variables-in-classes)
};

// How the culler should treat an object whose vertices a modifier moves.
//
// A deform moves vertices away from the asset's rest-pose bounds, and no envelope
// of a deformed object is computed yet, so the only policy that can be honoured is
// SkipCulling. `RestPose` would cull against bounds that no longer hold;
// `Conservative` and `Manual` both need data that does not exist yet (a computed
// envelope, or caller-supplied bounds). See `UnsupportedBoundsPolicy`.
enum class BoundsPolicy : std::uint8_t {
    RestPose,     // trust the asset's rest-pose bounds (unsafe for a real deform)
    SkipCulling,  // never cull and never use as an occluder
    Conservative, // cull against an envelope (follow-up; falls back to SkipCulling)
    Manual,       // trust caller-provided bounds
};

// Diagnostic spelling of a policy.
[[nodiscard]] constexpr std::string_view BoundsPolicyName(const BoundsPolicy policy) noexcept {
    switch (policy) {
        case BoundsPolicy::RestPose:     return "RestPose";
        case BoundsPolicy::SkipCulling:  return "SkipCulling";
        case BoundsPolicy::Conservative: return "Conservative";
        case BoundsPolicy::Manual:       return "Manual";
    }
    return "<unknown>";
}

// The policy an *active* deform cannot honour, or nullopt when the object already
// asked for SkipCulling (the one implemented policy).
//
// gather keeps `kDeformNoOccluder | kDeformNoCullTest` set for every active deform
// and never clears them from this component, so an unsupported request cannot
// silently shrink a deformed object's culling. The caller reports the returned
// policy once per object instead of letting it look honoured.
[[nodiscard]] constexpr std::optional<BoundsPolicy> UnsupportedBoundsPolicy(
    const BoundsPolicy requested) noexcept {
    switch (requested) {
        case BoundsPolicy::SkipCulling: return std::nullopt;
        case BoundsPolicy::RestPose:
        case BoundsPolicy::Conservative:
        case BoundsPolicy::Manual: return requested;
    }
    return requested;
}

// Per-object vertex-modifier stack.
//
// The stack is additive by default: the object's own entries apply to every
// submesh, and a submesh may append to or replace them. `nullopt` means "inherit
// the object stack", `Append{}` is identical to inherit, and `Replace{}` turns
// modifiers off for that submesh. Submesh indices are *object-local*, like
// MaterialOverride, because MeshRegistry reassigns global renderer ranges.
//
// The builtin skinning stage is not part of this component: gather prepends it to
// every skinned submesh, and it can be neither removed nor reordered here.
//
// Data component: dense storage.
struct GeometryModifier {
    // The short, stable name Describe() prints for this target. A plain
    // static member does not affect the object/data split, which is
    // is_base_of_v<Component, T>.
    static constexpr std::string_view kName = "GeometryModifier";
    static constexpr std::uint32_t kUnboundMesh =
        std::numeric_limits<std::uint32_t>::max();

    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    // Mesh this stack was authored against, or kUnboundMesh to adopt lazily.
    std::uint32_t mesh_id = kUnboundMesh;
    // Object-wide default, applied to every submesh that does not replace it.
    std::vector<ModifierInstance> modifiers;
    // Sparse, object-local; an unset entry inherits the object stack.
    std::vector<std::optional<SubMeshModifierStack>> submeshes;
    BoundsPolicy bounds = BoundsPolicy::SkipCulling;
    // Signatures of discrepancies already reported, so each distinct problem is
    // logged once instead of every frame.
    mutable std::unordered_set<std::uint64_t> reported_signatures;
    // NOLINTEND(misc-non-private-member-variables-in-classes)

    [[nodiscard]] bool ShouldReport(const std::uint64_t signature) const {
        return reported_signatures.insert(signature).second;
    }

    void Bind(const std::uint32_t bound_mesh_id) {
        mesh_id = bound_mesh_id;
        reported_signatures.clear();
    }

    void Unbind() {
        mesh_id = kUnboundMesh;
        reported_signatures.clear();
    }

    // Object-local submesh stack, or nullptr when the submesh inherits.
    [[nodiscard]] const SubMeshModifierStack* GetSubMeshStack(
        const std::uint32_t local_submesh) const {
        return local_submesh < submeshes.size() && submeshes[local_submesh].has_value()
            ? &*submeshes[local_submesh]
            : nullptr;
    }

    // Ensures the sparse vector covers `local_submesh` and returns its slot.
    SubMeshModifierStack& SetSubMeshStack(const std::uint32_t local_submesh) {
        if (local_submesh >= submeshes.size()) {
            submeshes.resize(static_cast<std::size_t>(local_submesh) + 1);
        }
        if (!submeshes[local_submesh].has_value()) {
            submeshes[local_submesh].emplace();
        }
        return *submeshes[local_submesh];
    }
};

// Visits the modifier instances that apply to one object-local submesh, in
// execution order. A Replace stack visits only its own entries; Append, nullopt
// and an out-of-range index visit the object stack first, then the submesh's own
// entries when present.
template <class Fn>
void ForEachModifierInstance(const GeometryModifier& modifier,
                             const std::uint32_t local_submesh,
                             Fn&& fn) {
    const SubMeshModifierStack* stack = modifier.GetSubMeshStack(local_submesh);
    const bool replaces = stack != nullptr && stack->mode == ModifierStackMode::Replace;
    if (!replaces) {
        for (const ModifierInstance& instance : modifier.modifiers) {
            std::invoke(fn, instance);
        }
    }
    if (stack != nullptr) {
        for (const ModifierInstance& instance : stack->modifiers) {
            std::invoke(fn, instance);
        }
    }
}

} // namespace VulkanEngine::Components
