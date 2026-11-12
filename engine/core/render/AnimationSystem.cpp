module;

#include <logging/logging_macros.hpp>

module VulkanEngine.AnimationSystem;

import std;

import logiface;

import VulkanEngine.ECS.ComponentRegistry;
import VulkanEngine.Components.AnimationPlayer;
import VulkanEngine.Components.SkeletonPose;
import VulkanEngine.Mesh.MeshTypes;
import VulkanEngine.Mesh.SkeletalAnimation;

namespace VulkanEngine {

namespace {

// Distinct causes a player can be skipped for. Each is reported once per player
// (the component remembers what it already reported), and the reason is folded
// into the signature so two different mismatches both surface.
enum class RejectReason : std::uint64_t {
    NoAsset = 1,
    PoseHasNoAsset = 2,
    AssetMismatch = 3,
    ClipOutOfRange = 4,
    SkeletonOutOfRange = 5,
    ClipSkeletonMismatch = 6,
};

// A distinct signature per (cause, detail...) so the component's once-per-cause
// set reports each mismatch exactly once. The mixing is the usual 64-bit
// avalanche constant; collisions only cost a suppressed second diagnostic.
[[nodiscard]] constexpr std::uint64_t RejectSignature(const RejectReason reason,
                                                      const std::uint64_t a = 0,
                                                      const std::uint64_t b = 0) noexcept {
    constexpr std::uint64_t kMix = 0x9E3779B97F4A7C15ull;
    std::uint64_t h = static_cast<std::uint64_t>(reason) * kMix;
    h ^= a + kMix + (h << 6) + (h >> 2);
    h ^= b + kMix + (h << 6) + (h >> 2);
    return h;
}

// The clip a player may sample, or nullptr when the player/pose pair is not
// consistent. A mismatch is never resolved by guessing: sampling a clip from one
// asset into another asset's pose would drive matrices from the wrong skeleton.
[[nodiscard]] const AnimationClip* ResolveClip(Components::AnimationPlayer& player,
                                              const Components::SkeletonPose& pose,
                                              const std::uint64_t entity_id) {
    const auto reject = [&player, entity_id](const RejectReason reason,
                                             const std::uint64_t a,
                                             const std::uint64_t b,
                                             const std::string& message) -> const AnimationClip* {
        if (player.ShouldReport(RejectSignature(reason, a, b))) {
            LOGIFACE_LOG(warn, "AnimationSystem: entity " + std::to_string(entity_id) + " " +
                message + "; the player is ignored");
        }
        return nullptr;
    };

    if (player.asset == nullptr) {
        return reject(RejectReason::NoAsset, 0, 0, "has an AnimationPlayer with no asset");
    }
    if (pose.asset == nullptr) {
        return reject(RejectReason::PoseHasNoAsset, 0, 0,
                      "has an AnimationPlayer but its SkeletonPose has no asset");
    }
    if (player.asset != pose.asset) {
        // Pointer identity is deliberate: the two components must share one
        // SkeletalAsset, otherwise their skeleton/skin indices mean different
        // things even when both describe the same mesh.
        return reject(RejectReason::AssetMismatch, 0, 0,
                      "holds a different SkeletalAsset than its SkeletonPose");
    }
    if (player.clip_index >= player.asset->animations.size()) {
        return reject(RejectReason::ClipOutOfRange, player.clip_index, 0,
                      "names clip " + std::to_string(player.clip_index) + " but the asset has " +
                      std::to_string(player.asset->animations.size()) + " clips");
    }
    const AnimationClip& clip = player.asset->animations[player.clip_index];
    if (pose.skeleton_index >= player.asset->skeletons.size()) {
        return reject(RejectReason::SkeletonOutOfRange, pose.skeleton_index, 0,
                      "poses skeleton " + std::to_string(pose.skeleton_index) +
                      " but the asset has " + std::to_string(player.asset->skeletons.size()) +
                      " skeletons");
    }
    if (clip.skeleton != pose.skeleton_index) {
        return reject(RejectReason::ClipSkeletonMismatch, clip.skeleton, pose.skeleton_index,
                      "plays a clip for skeleton " + std::to_string(clip.skeleton) +
                      " into skeleton " + std::to_string(pose.skeleton_index));
    }
    return &clip;
}

} // namespace

std::size_t AnimationSystem::Update(ComponentRegistry& registry, const float delta_time) {
    std::size_t rebuilt = 0;

    registry.ForEach<Components::AnimationPlayer, Components::SkeletonPose>(
        [&rebuilt, delta_time](Entity& entity, Components::AnimationPlayer& player,
                               Components::SkeletonPose& pose) {
            const AnimationClip* const clip =
                ResolveClip(player, pose, static_cast<std::uint64_t>(entity.GetId().index));
            if (clip == nullptr) {
                // An ignored player leaves the pose exactly as it was, except when the
                // pose itself is stale: `dirty` (a fresh or requested rebuild) or an
                // `asset`/`skeleton_index` swap that this branch is the only chance to
                // notice for a player-owned pose. Without the second trigger the pose
                // would keep the previous asset's world/palettes while gather copies
                // them for the new mesh.
                if (pose.dirty || !pose.MatchesBuiltState()) {
                    pose.Rebuild();
                    // The bind pose is not a sample of the player's clip, so the
                    // recorded "already sampled" state no longer describes the pose.
                    // Without this a pair that recovers with the same asset, clip
                    // and time would match the settle check and never re-sample.
                    player.sampled_asset = nullptr;
                    player.sampled_clip_index = Components::AnimationPlayer::kNotSampled;
                    player.sampled_time = 0.0f;
                    ++rebuilt;
                }
                return;
            }

            // Bound `time` here as well as inside SampleClip so the component's own
            // value stays readable: an app can inspect `time`, and a looping player
            // must not accumulate an unbounded time that loses float precision
            // after a few hours of playback.
            if (clip->duration > 0.0f) {
                if (player.playing) {
                    player.time += player.speed * delta_time;
                }
                if (player.loop) {
                    player.time = std::fmod(player.time, clip->duration);
                    if (player.time < 0.0f) player.time += clip->duration;
                } else {
                    player.time = std::clamp(player.time, 0.0f, clip->duration);
                }
            } else {
                // A clip with no duration has no time domain to advance through:
                // hold it at 0 instead of integrating without bound and re-sampling
                // an identical pose every frame.
                player.time = 0.0f;
            }

            // A paused, frozen or one-shot-finished player that is already settled
            // costs nothing: the pose is re-sampled only when the player's asset,
            // clip or time changed, or when someone else rebuilt the pose.
            // `sampled_pose_version` is what detects that last case: a pose rebuilt
            // outside this system (a manual `SkeletonPose::Rebuild`) clears `dirty`
            // itself, so trusting `dirty` alone would leave it stuck at the bind
            // pose.
            if (!pose.dirty && player.sampled_asset == player.asset.get() &&
                player.sampled_clip_index == player.clip_index &&
                player.sampled_time == player.time &&
                player.sampled_pose_version == pose.version) {
                return;
            }

            const Skeleton& skeleton = player.asset->skeletons[clip->skeleton];
            pose.local.resize(skeleton.joints.size());
            SampleClip(*clip, skeleton, player.time, pose.local, player.loop);
            // The bind-pose fill inside SampleClip is what keeps a previous clip's
            // unanimated joints from leaking into the new clip, so a clip switch
            // needs no extra reset here.
            pose.RebuildFromLocal();
            player.sampled_asset = player.asset.get();
            player.sampled_clip_index = player.clip_index;
            player.sampled_time = player.time;
            player.sampled_pose_version = pose.version;
            ++rebuilt;
        });

    // Poses without a player: the bind pose is the only thing to rebuild. Both
    // signals matter — `dirty` for a fresh/requested rebuild, and the recorded
    // build identity for an `asset`/`skeleton_index` swap, which no other code
    // marks because the component's fields are plain data.
    registry.ForEach<Components::SkeletonPose>(
        [&rebuilt](Entity& entity, Components::SkeletonPose& pose) {
            if (!pose.dirty && pose.MatchesBuiltState()) return;
            // The player case was already handled above; rebuilding the bind pose
            // here as well would overwrite the sampled pose every frame.
            if (entity.GetComponent<Components::AnimationPlayer>() != nullptr) return;
            pose.Rebuild();
            ++rebuilt;
        });

    return rebuilt;
}

} // namespace VulkanEngine
