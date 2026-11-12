module;

module VulkanEngine.Components.SkeletonPose;

import std;

import VulkanEngine.Mesh.MeshTypes;
import VulkanEngine.Mesh.SkeletalAnimation;

namespace VulkanEngine::Components {

namespace {

[[nodiscard]] constexpr std::array<float, 16> IdentityMatrix() noexcept {
    return {1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f};
}

} // namespace

void SkeletonPose::Rebuild() {
    // Fill `local` from the bind pose, then let RebuildFromLocal do the rest. An
    // unresolvable pose (no asset, or a `skeleton_index` outside it) falls through
    // to the same path, so it also records the identity it was built from: a
    // consumer that keys on `MatchesBuiltState` must not see such a pose as
    // permanently stale and rebuild it every frame.
    if (asset && skeleton_index < asset->skeletons.size()) {
        const Skeleton& skeleton = asset->skeletons[skeleton_index];
        local.resize(skeleton.joints.size());
        for (std::size_t i = 0; i < skeleton.joints.size(); ++i) {
            local[i] = skeleton.joints[i].local_bind;
        }
    }
    RebuildFromLocal();
}

void SkeletonPose::RebuildFromLocal() {
    // Record the input identity before any early return, so `MatchesBuiltState`
    // always describes the state the pose is actually in (including "no asset").
    built_asset = asset.get();
    built_skeleton_index = skeleton_index;

    if (!asset || skeleton_index >= asset->skeletons.size()) {
        local.clear();
        world.clear();
        palettes.clear();
        dirty = false;
        ++version;
        return;
    }

    const Skeleton& skeleton = asset->skeletons[skeleton_index];
    if (local.size() != skeleton.joints.size()) {
        // A caller sampled into a buffer sized for another skeleton (or cleared
        // it): fall back to the bind pose rather than reading past the end or
        // composing a partial pose that would put joints at the origin.
        local.resize(skeleton.joints.size());
        for (std::size_t i = 0; i < skeleton.joints.size(); ++i) {
            local[i] = skeleton.joints[i].local_bind;
        }
    }

    world.assign(skeleton.joints.size(), IdentityMatrix());
    ComputeWorld(skeleton, local, world);

    // Cleared first: a skin that was bound to a previous `skeleton_index` must
    // not keep a stale palette, and an unbound skin's entry has to be empty so
    // GetPalette reports it as unbound.
    palettes.assign(asset->skins.size(), {});
    for (std::size_t s = 0; s < asset->skins.size(); ++s) {
        const Skin& skin = asset->skins[s];
        if (skin.skeleton != skeleton_index) continue;
        palettes[s].assign(skin.joints.size(), IdentityMatrix());
        ComputePalette(skin, world, palettes[s]);
    }

    dirty = false;
    ++version;
}

std::span<const std::array<float, 16>> SkeletonPose::GetPalette(const std::uint32_t skin_index) const {
    if (skin_index >= palettes.size()) return {};
    return palettes[skin_index];
}

} // namespace VulkanEngine::Components
