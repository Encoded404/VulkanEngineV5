module;

export module VulkanEngine.Components.SkeletonPose;

import std;

import VulkanEngine.ECS.ComponentRegistry;
import VulkanEngine.Mesh.MeshTypes;
import VulkanEngine.Mesh.SkeletalAnimation;

export namespace VulkanEngine::Components {

// Bind-pose (and, from the animation runtime on, sampled) pose of one skeleton.
//
// The component never aliases `MeshData`/`MeshRegistry` storage: `asset` is a
// refcounted copy, so the pose keeps working after the source mesh is evicted or
// re-registered.
//
// `palettes` has one entry per skin in `asset->skins`. Only skins whose
// `Skin::skeleton == skeleton_index` get a populated palette; every other entry
// is empty and GetPalette returns an empty span for it, so a multi-skeleton asset
// can never read another skeleton's world array.
struct SkeletonPose {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::shared_ptr<const SkeletalAsset> asset;
    std::uint32_t skeleton_index = kNoSkeleton; // into asset->skeletons
    std::vector<SubMeshTransform> local;
    std::vector<std::array<float, 16>> world;
    std::vector<std::vector<std::array<float, 16>>> palettes;
    std::uint32_t version = 0;
    bool dirty = true;
    // NOLINTEND(misc-non-private-member-variables-in-classes)

    // Recomputes the bind pose (local = SkeletonJoint::local_bind, world with the
    // SkeletonRoot transforms, order-independent) and the palettes of every skin
    // bound to `skeleton_index`. Bumps `version` and clears `dirty`.
    void Rebuild();

    // The palette of `skin_index`, empty when that skin is not bound to this
    // pose or the index is out of range.
    [[nodiscard]] std::span<const std::array<float, 16>> GetPalette(std::uint32_t skin_index) const;
};

} // namespace VulkanEngine::Components
