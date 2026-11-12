module;

export module VulkanEngine.Components.AnimationPlayer;

import std;

import VulkanEngine.ECS.ComponentRegistry;
import VulkanEngine.Mesh.MeshTypes;
import VulkanEngine.Mesh.SkeletalAnimation;

export namespace VulkanEngine::Components {

// Clip playback state for one skinned object.
//
// The player is deliberately a *pair* with `SkeletonPose`, never a replacement
// for it: the pose owns the sampled local/world/palette arrays and the version
// gather watches, while the player only says which clip to sample and at what
// time. Both components carry the same shared `SkeletalAsset`, so the pair
// outlives registry eviction of the source mesh and two objects can share one
// asset.
//
// Playback is resolved by `AnimationSystem`: a player whose clip does not belong
// to the pose's skeleton is ignored (and reported once) rather than driving
// another skeleton's matrix arrays.
struct AnimationPlayer {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::shared_ptr<const SkeletalAsset> asset;
    std::uint32_t clip_index = 0; // into asset->animations
    // Seconds into the clip. Advanced by speed * delta_time, wrapped (loop) or
    // clamped (no loop) against the clip duration.
    float time = 0.0f;
    // Playback rate. 0 freezes time but still poses the object at `time`.
    float speed = 1.0f;
    bool loop = true;
    // False pauses advancement. The object still poses at `time`, so a paused
    // player that is scrubbed re-samples on the next update.
    bool playing = true;
    // Discrepancies already reported for this player, so a mismatched asset or
    // clip is logged once instead of every frame.
    mutable std::unordered_set<std::uint64_t> reported_signatures;
    // Playback bookkeeping, written by the animation system: which asset, clip,
    // time and resulting pose version the pose was last sampled from. Comparing
    // all of them against the current state is what makes an asset swap, a clip
    // switch or a scrub re-sample without rebuilding a settled pose every frame,
    // and `sampled_pose_version` catches a pose someone else rebuilt (a manual
    // `SkeletonPose::Rebuild`) even though nothing else changed.
    //
    // `sampled_asset` is an identity tag only, never dereferenced, so it does not
    // keep the asset alive or read it after the pair was re-pointed.
    const SkeletalAsset* sampled_asset = nullptr;
    std::uint32_t sampled_clip_index = kNotSampled;
    float sampled_time = 0.0f;
    std::uint32_t sampled_pose_version = 0;
    // NOLINTEND(misc-non-private-member-variables-in-classes)

    // Never-sampled marker for `sampled_clip_index`; no clip can hold this index.
    static constexpr std::uint32_t kNotSampled = std::numeric_limits<std::uint32_t>::max();

    [[nodiscard]] bool ShouldReport(const std::uint64_t signature) const {
        return reported_signatures.insert(signature).second;
    }
};

} // namespace VulkanEngine::Components
