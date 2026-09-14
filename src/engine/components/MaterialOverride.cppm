module;

export module VulkanEngine.Components.MaterialOverride;

import std;

import VulkanEngine.ECS.ComponentRegistry;
import VulkanEngine.MaterialManager.MaterialId;

export namespace VulkanEngine::Components {

// Per-object, per-submesh material override.
//
// When present on an entity that owns a MeshReference (or DynamicMesh), the
// listed submeshes render with the referenced material instead of the mesh
// asset's own material. The override is keyed by *object-local* submesh index
// (0 .. submesh_count-1), never by the global renderer submesh index, because
// MeshRegistry reassigns global ranges on re-residency.
//
// Overrides are sparse: any index not set (or beyond the stored vector) falls
// back to the asset material. References are generation-checked (MaterialRef)
// so a destroyed-then-reused material slot is detected rather than silently
// aliasing a different material.
//
// The owning mesh is fingerprinted via `mesh_id`. While unbound (kUnboundMesh)
// the override adopts the owner's mesh on first resolve. If the owner's mesh
// later changes, the override is ignored and reported once instead of being
// applied to a different submesh layout.
//
// Deliberately no GetFields(): `submeshes` is a variable-length vector and is
// not representable by the field-reflection system. This component is stored
// through the AoS path (like DynamicMesh).
class MaterialOverride : public VulkanEngine::Component {
public:
    static constexpr std::uint32_t kUnboundMesh =
        std::numeric_limits<std::uint32_t>::max();

    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    // Mesh this override was authored against, or kUnboundMesh to adopt lazily.
    std::uint32_t mesh_id = kUnboundMesh;

    // Per object-local submesh index; unset entries are MaterialRef::Unset().
    std::vector<MaterialManager::MaterialRef> submeshes;

    // Signatures of discrepancies already reported, so each distinct problem is
    // logged exactly once instead of every frame. Bounded by the number of
    // distinct problems (submeshes + mesh-level), not by frame count.
    mutable std::unordered_set<std::uint64_t> reported_signatures;
    // NOLINTEND(misc-non-private-member-variables-in-classes)

    // Returns true the first time a discrepancy signature is observed.
    [[nodiscard]] bool ShouldReport(const std::uint64_t signature) const {
        return reported_signatures.insert(signature).second;
    }

    // Pin this override to a specific mesh. Any outstanding diagnostic state is
    // reset so a fresh mismatch is reported.
    void Bind(const std::uint32_t bound_mesh_id) {
        mesh_id = bound_mesh_id;
        reported_signatures.clear();
    }

    void Unbind() {
        mesh_id = kUnboundMesh;
        reported_signatures.clear();
    }

    void Set(const std::uint32_t submesh_index, const MaterialManager::MaterialRef ref) {
        if (submesh_index >= submeshes.size()) {
            submeshes.resize(static_cast<std::size_t>(submesh_index) + 1);
        }
        submeshes[submesh_index] = ref;
    }

    void Clear(const std::uint32_t submesh_index) {
        if (submesh_index < submeshes.size()) {
            submeshes[submesh_index] = MaterialManager::MaterialRef{};
        }
    }

    void ClearAll() {
        submeshes.clear();
        reported_signatures.clear();
    }

    [[nodiscard]] bool Has(const std::uint32_t submesh_index) const {
        return submesh_index < submeshes.size() && submeshes[submesh_index].IsSet();
    }

    [[nodiscard]] MaterialManager::MaterialRef Get(const std::uint32_t submesh_index) const {
        return submesh_index < submeshes.size()
            ? submeshes[submesh_index]
            : MaterialManager::MaterialRef{};
    }
};

} // namespace VulkanEngine::Components
