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
    // The short, stable name Describe() prints for this target. A plain
    // static member does not affect the object/data split, which is
    // is_base_of_v<Component, T>.
    static constexpr std::string_view kName = "SkeletonPose";
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::shared_ptr<const SkeletalAsset> asset;
    std::uint32_t skeleton_index = kNoSkeleton; // into asset->skeletons
    std::vector<SubMeshTransform> local;
    std::vector<std::array<float, 16>> world;
    std::vector<std::vector<std::array<float, 16>>> palettes;
    std::uint32_t version = 0;
    bool dirty = true;
    // Bookkeeping, written by Rebuild/RebuildFromLocal: which asset and skeleton the
    // current `world`/`palettes` were computed from. `built_asset` is an identity
    // tag only and is never dereferenced, so re-pointing `asset` cannot leave a
    // dangling read.
    const SkeletalAsset* built_asset = nullptr;
    std::uint32_t built_skeleton_index = kNoSkeleton;
    // NOLINTEND(misc-non-private-member-variables-in-classes)

    // True when `world`/`palettes` were computed from the `asset`/`skeleton_index`
    // the component currently names. A consumer must check this rather than trust
    // `dirty`, which the rebuild itself clears and any other caller may have.
    [[nodiscard]] bool MatchesBuiltState() const noexcept {
        return built_asset == asset.get() && built_skeleton_index == skeleton_index;
    }

    // Recomputes the bind pose (local = SkeletonJoint::local_bind, world with the
    // SkeletonRoot transforms, order-independent) and the palettes of every skin
    // bound to `skeleton_index`. Bumps `version` and clears `dirty`. A pose with no
    // asset, or whose `skeleton_index` is outside it, is left cleared rather than
    // half-built.
    //
    // While an `AnimationPlayer` is attached, the next animation update samples its
    // clip over this pose: the player owns the pose, so a manual bind-pose reset
    // does not survive. Detaching the player stops sampling but leaves the last
    // sampled pose in place; call `Rebuild()` (or mark `dirty`) after detaching to
    // return the object to its rest pose.
    void Rebuild();

    // Recomputes `world` and the palettes from the current `local`, which is what
    // an animation system fills with sampled joint transforms. A `local` whose
    // size does not match the skeleton is reset to the bind pose rather than read
    // past its end. Bumps `version` and clears `dirty`.
    void RebuildFromLocal();

    // The palette of `skin_index`, empty when that skin is not bound to this
    // pose or the index is out of range.
    [[nodiscard]] std::span<const std::array<float, 16>> GetPalette(std::uint32_t skin_index) const;
};

} // namespace VulkanEngine::Components
