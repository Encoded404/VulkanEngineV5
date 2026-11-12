// Device-free tests for the clip playback system.
//
// AnimationSystem is pure CPU: it advances AnimationPlayer components, samples
// clips into SkeletonPose::local and rebuilds world/palettes. Nothing here creates
// a Vulkan device, so the whole playback contract (time advance and wrap, looping
// vs one-shot, pause, speed, bind-pose fallback, clip switching and asset
// ownership) is asserted directly.

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <memory>
#include <vector>

import std;

import VulkanEngine.Mesh.MeshTypes;
import VulkanEngine.Mesh.SkeletalAnimation;
import VulkanEngine.GpuResources.MeshData;
import VulkanEngine.ECS.ComponentRegistry;
import VulkanEngine.Components.SkeletonPose;
import VulkanEngine.Components.AnimationPlayer;
import VulkanEngine.AnimationSystem;

namespace {

using VulkanEngine::AnimationChannel;
using VulkanEngine::AnimationClip;
using VulkanEngine::AnimationInterpolation;
using VulkanEngine::AnimationKey;
using VulkanEngine::AnimationPath;
using VulkanEngine::AnimationSystem;
using VulkanEngine::ComponentRegistry;
using VulkanEngine::Entity;
using VulkanEngine::GpuResources::MeshData;
using VulkanEngine::MeshVertexVec3;
using VulkanEngine::Skeleton;
using VulkanEngine::SkeletonJoint;
using VulkanEngine::SkeletonRoot;
using VulkanEngine::Skin;
using VulkanEngine::SubMeshTransform;
using VulkanEngine::kNoSkeleton;
using VulkanEngine::Components::AnimationPlayer;
using VulkanEngine::Components::SkeletonPose;

constexpr float kEps = 1e-5f;

[[nodiscard]] constexpr std::array<float, 16> Identity() {
    return {1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f};
}

[[nodiscard]] SubMeshTransform Trs(const float x, const float y, const float z) {
    SubMeshTransform t{};
    t.position = MeshVertexVec3{x, y, z};
    return t;
}

[[nodiscard]] AnimationChannel TranslationChannel(const std::uint32_t joint,
                                                  const std::vector<AnimationKey>& keys) {
    AnimationChannel channel{};
    channel.joint = joint;
    channel.path = AnimationPath::Translation;
    channel.interpolation = AnimationInterpolation::Linear;
    channel.keys = keys;
    return channel;
}

// A joint chain with a bind-pose offset of +1 on X per joint:
//
//   0 (root) -> 1 -> 2
//              \--> 3
//
// Joint 1 is never channelled, so its bind offset stays in every world sum. That
// makes a sampled joint's world X equal to 1 + (its own clip value), which is
// always distinct from the bind pose's 2.
[[nodiscard]] Skeleton MakeChainSkeleton() {
    Skeleton skeleton;
    skeleton.joints.resize(4);
    skeleton.joints[0].parent = kNoSkeleton;
    skeleton.joints[0].local_bind = Trs(0.0f, 0.0f, 0.0f);
    skeleton.joints[1].parent = 0;
    skeleton.joints[1].local_bind = Trs(1.0f, 0.0f, 0.0f);
    skeleton.joints[2].parent = 1;
    skeleton.joints[2].local_bind = Trs(1.0f, 0.0f, 0.0f);
    skeleton.joints[3].parent = 1;
    skeleton.joints[3].local_bind = Trs(1.0f, 0.0f, 0.0f);
    skeleton.roots.push_back(SkeletonRoot{.joint = 0, .transform = Identity()});
    skeleton.names = {"root", "spine", "arm", "leg"};
    return skeleton;
}

// Animates two joints (2 -> 4x, 3 -> 3x). World X is therefore 1 + 4t for joint 2
// and 1 + 3t for joint 3.
[[nodiscard]] AnimationClip MakeWideClip() {
    AnimationClip clip;
    clip.name = "wide";
    clip.skeleton = 0;
    clip.duration = 1.0f;
    clip.channels = {
        TranslationChannel(2, {{.time = 0.0f, .value = {0.0f, 0.0f, 0.0f, 0.0f}},
                               {.time = 1.0f, .value = {4.0f, 0.0f, 0.0f, 0.0f}}}),
        TranslationChannel(3, {{.time = 0.0f, .value = {0.0f, 0.0f, 0.0f, 0.0f}},
                               {.time = 1.0f, .value = {3.0f, 0.0f, 0.0f, 0.0f}}}),
    };
    return clip;
}

// Animates only joint 2, held at x = 10 for the whole clip. Switching from the
// wide clip must put joint 3 back at its bind offset.
[[nodiscard]] AnimationClip MakeNarrowClip() {
    AnimationClip clip;
    clip.name = "narrow";
    clip.skeleton = 0;
    clip.duration = 1.0f;
    clip.channels = {
        TranslationChannel(2, {{.time = 0.0f, .value = {10.0f, 0.0f, 0.0f, 0.0f}},
                               {.time = 1.0f, .value = {10.0f, 0.0f, 0.0f, 0.0f}}}),
    };
    return clip;
}

[[nodiscard]] std::shared_ptr<const VulkanEngine::SkeletalAsset> MakeChainAsset() {
    MeshData data;
    data.skeletons.push_back(MakeChainSkeleton());
    Skin skin{};
    skin.skeleton = 0;
    skin.joints = {0, 1, 2, 3};
    skin.inverse_bind.assign(4, Identity());
    data.skins.push_back(skin);
    data.animations.push_back(MakeWideClip());
    data.animations.push_back(MakeNarrowClip());
    return VulkanEngine::MakeSkeletalAsset(data);
}

// One entity owning a pose and a player, both sharing one asset. The pose starts
// dirty (as a freshly added component does), so the first Update samples.
struct Playback {
    ComponentRegistry registry{};
    Entity* entity = nullptr;
    AnimationPlayer* player = nullptr;
    SkeletonPose* pose = nullptr;

    explicit Playback(const std::shared_ptr<const VulkanEngine::SkeletalAsset>& asset,
                      const std::uint32_t clip_index = 0) {
        entity = &registry.CreateEntity();
        auto& pose_component = registry.AddComponent<SkeletonPose>(*entity);
        pose_component.asset = asset;
        pose_component.skeleton_index = 0;

        auto& player_component = registry.AddComponent<AnimationPlayer>(*entity);
        player_component.asset = asset;
        player_component.clip_index = clip_index;

        pose = &pose_component;
        player = &player_component;
    }
};

// World-space X of one joint, which composes the whole chain above it.
[[nodiscard]] float WorldX(const SkeletonPose& pose, const std::size_t joint) {
    return pose.world.at(joint)[12];
}

// ── time advance ────────────────────────────────────────────────────────────

TEST(AnimationSystem, AdvancesAndWrapsLoopingPlayback) {
    const auto asset = MakeChainAsset();
    Playback playback(asset);
    AnimationSystem system;

    EXPECT_EQ(system.Update(playback.registry, 0.25f), 1u);
    EXPECT_NEAR(playback.player->time, 0.25f, kEps);
    EXPECT_NEAR(WorldX(*playback.pose, 2), 2.0f, kEps) << "joint 2 samples 1.0 at t = 0.25";

    EXPECT_EQ(system.Update(playback.registry, 0.25f), 1u);
    EXPECT_NEAR(playback.player->time, 0.5f, kEps);
    EXPECT_NEAR(WorldX(*playback.pose, 2), 3.0f, kEps) << "joint 2 samples 2.0 at t = 0.5";

    // Past the end the looping player wraps instead of clamping, so time stays
    // inside the clip and never grows without bound.
    EXPECT_EQ(system.Update(playback.registry, 0.75f), 1u);
    EXPECT_NEAR(playback.player->time, 0.25f, kEps);
    EXPECT_NEAR(WorldX(*playback.pose, 2), 2.0f, kEps);
}

TEST(AnimationSystem, OneShotClampsToTheFinalPose) {
    const auto asset = MakeChainAsset();
    Playback playback(asset);
    playback.player->loop = false;
    AnimationSystem system;

    EXPECT_EQ(system.Update(playback.registry, 2.0f), 1u);
    EXPECT_NEAR(playback.player->time, 1.0f, kEps);
    // The end of the clip, not its start: wrapping here would snap a finished
    // one-shot animation back to the first key.
    EXPECT_NEAR(WorldX(*playback.pose, 2), 5.0f, kEps);

    // Already at the end: the advance is clamped away, so the pose is not rebuilt.
    const std::uint32_t version = playback.pose->version;
    EXPECT_EQ(system.Update(playback.registry, 1.0f), 0u);
    EXPECT_EQ(playback.pose->version, version);
    EXPECT_NEAR(playback.player->time, 1.0f, kEps);
}

TEST(AnimationSystem, NegativeTimeWrapsForward) {
    const auto asset = MakeChainAsset();
    Playback playback(asset);
    AnimationSystem system;

    EXPECT_EQ(system.Update(playback.registry, -0.25f), 1u);
    EXPECT_NEAR(playback.player->time, 0.75f, kEps);
    EXPECT_NEAR(WorldX(*playback.pose, 2), 4.0f, kEps);
}

TEST(AnimationSystem, ZeroSpeedFreezesAtTheCurrentTime) {
    const auto asset = MakeChainAsset();
    Playback playback(asset);
    playback.player->speed = 0.0f;
    AnimationSystem system;

    // The first update still samples, so a frozen player poses at `time` instead
    // of leaving the pose where it was.
    EXPECT_EQ(system.Update(playback.registry, 0.5f), 1u);
    EXPECT_NEAR(playback.player->time, 0.0f, kEps);
    EXPECT_NEAR(WorldX(*playback.pose, 2), 1.0f, kEps) << "the clip samples 0 at t = 0";

    // Frozen and settled: the pose is not rebuilt every frame.
    const std::uint32_t version = playback.pose->version;
    EXPECT_EQ(system.Update(playback.registry, 0.5f), 0u);
    EXPECT_EQ(playback.pose->version, version);
}

TEST(AnimationSystem, PausedPlayerDoesNotAdvanceButStillPosesAScrub) {
    const auto asset = MakeChainAsset();
    Playback playback(asset);
    AnimationSystem system;

    ASSERT_EQ(system.Update(playback.registry, 0.5f), 1u);
    const float sampled_x = WorldX(*playback.pose, 2);

    playback.player->playing = false;
    EXPECT_EQ(system.Update(playback.registry, 0.5f), 0u);
    EXPECT_NEAR(playback.player->time, 0.5f, kEps);
    EXPECT_NEAR(WorldX(*playback.pose, 2), sampled_x, kEps);

    // Scrubbing a paused player re-samples on the next update: the player records
    // what the pose was last sampled from, so no explicit dirty flag is needed.
    playback.player->time = 0.75f;
    EXPECT_EQ(system.Update(playback.registry, 0.5f), 1u);
    EXPECT_NEAR(WorldX(*playback.pose, 2), 4.0f, kEps);
}

TEST(AnimationSystem, VersionBumpsOnlyWhenThePoseIsRebuilt) {
    const auto asset = MakeChainAsset();
    Playback playback(asset);
    const std::uint32_t start = playback.pose->version;
    AnimationSystem system;

    ASSERT_EQ(system.Update(playback.registry, 0.1f), 1u);
    EXPECT_EQ(playback.pose->version, start + 1);

    ASSERT_EQ(system.Update(playback.registry, 0.1f), 1u);
    EXPECT_EQ(playback.pose->version, start + 2);

    // A zero delta cannot move a clip, so gather copies the pose already built.
    EXPECT_EQ(system.Update(playback.registry, 0.0f), 0u);
    EXPECT_EQ(playback.pose->version, start + 2);
}

// ── clip switching and bind-pose fallback ───────────────────────────────────

TEST(AnimationSystem, SwitchingToANarrowerClipRestoresBindPose) {
    const auto asset = MakeChainAsset();
    Playback playback(asset);
    AnimationSystem system;

    ASSERT_EQ(system.Update(playback.registry, 0.5f), 1u);
    // The wide clip moves both of its channelled joints off their bind pose.
    EXPECT_NEAR(playback.pose->local.at(1).position.x, 1.0f, kEps) << "joint 1 is never channelled";
    EXPECT_NEAR(playback.pose->local.at(2).position.x, 2.0f, kEps);
    EXPECT_NEAR(playback.pose->local.at(3).position.x, 1.5f, kEps);

    playback.player->clip_index = 1;
    playback.player->time = 0.5f;
    ASSERT_EQ(system.Update(playback.registry, 0.0f), 1u);

    // The narrow clip only channels joint 2; the bind-pose fill inside SampleClip
    // is what puts joint 3 back where the rest pose has it.
    EXPECT_NEAR(playback.pose->local.at(2).position.x, 10.0f, kEps);
    EXPECT_NEAR(playback.pose->local.at(3).position.x, 1.0f, kEps);
    EXPECT_NEAR(WorldX(*playback.pose, 3), 2.0f, kEps);
}

TEST(AnimationSystem, PoseWithoutAPlayerRebuildsTheBindPose) {
    const auto asset = MakeChainAsset();
    ComponentRegistry registry{};
    Entity& entity = registry.CreateEntity();
    auto& pose = registry.AddComponent<SkeletonPose>(entity);
    pose.asset = asset;
    pose.skeleton_index = 0;
    pose.dirty = true;

    AnimationSystem system;
    EXPECT_EQ(system.Update(registry, 0.5f), 1u);
    EXPECT_FALSE(pose.dirty);
    EXPECT_NEAR(WorldX(pose, 0), 0.0f, kEps);
    EXPECT_NEAR(WorldX(pose, 2), 2.0f, kEps) << "bind pose: +1 per joint";
    EXPECT_NEAR(WorldX(pose, 3), 2.0f, kEps);

    // Settled: a second update does nothing.
    EXPECT_EQ(system.Update(registry, 0.5f), 0u);
}

TEST(AnimationSystem, AnUnresolvablePoseSettlesInsteadOfRebuildingEveryFrame) {
    const auto asset = MakeChainAsset();
    ComponentRegistry registry{};
    Entity& entity = registry.CreateEntity();
    auto& pose = registry.AddComponent<SkeletonPose>(entity);
    pose.asset = asset;
    pose.skeleton_index = 7; // outside the asset

    AnimationSystem system;
    EXPECT_EQ(system.Update(registry, 0.0f), 1u);
    EXPECT_TRUE(pose.world.empty());
    // The rebuild records the identity it ran on, so an unresolvable pose settles
    // like any other instead of being reported and rebuilt every frame.
    EXPECT_EQ(system.Update(registry, 0.0f), 0u);

    // Pointing it at a real skeleton is an identity change: rebuild.
    pose.skeleton_index = 0;
    EXPECT_EQ(system.Update(registry, 0.0f), 1u);
    EXPECT_EQ(pose.world.size(), 4u);
    EXPECT_NEAR(WorldX(pose, 2), 2.0f, kEps);
}

TEST(AnimationSystem, DetachingThePlayerFreezesThePoseUntilRebuilt) {
    const auto asset = MakeChainAsset();
    Playback playback(asset);
    AnimationSystem system;

    ASSERT_EQ(system.Update(playback.registry, 0.5f), 1u);
    EXPECT_NEAR(WorldX(*playback.pose, 2), 3.0f, kEps);

    // Detaching the player stops sampling but keeps the last sampled pose: an app
    // that wants the rest pose has to rebuild it.
    playback.registry.RemoveComponent<AnimationPlayer>(*playback.entity);
    EXPECT_EQ(system.Update(playback.registry, 0.5f), 0u);
    EXPECT_NEAR(WorldX(*playback.pose, 2), 3.0f, kEps);

    playback.pose->Rebuild();
    EXPECT_NEAR(WorldX(*playback.pose, 2), 2.0f, kEps);
    EXPECT_EQ(system.Update(playback.registry, 0.5f), 0u) << "nothing else re-samples it";
    EXPECT_NEAR(WorldX(*playback.pose, 2), 2.0f, kEps);
}

TEST(AnimationSystem, PoseWithNoAssetIsLeftEmpty) {
    ComponentRegistry registry{};
    Entity& entity = registry.CreateEntity();
    auto& pose = registry.AddComponent<SkeletonPose>(entity);
    pose.dirty = true;

    AnimationSystem system;
    EXPECT_EQ(system.Update(registry, 0.5f), 1u);
    EXPECT_TRUE(pose.world.empty());
    EXPECT_FALSE(pose.dirty);
}

// ── player / pose consistency ───────────────────────────────────────────────

TEST(AnimationSystem, PlayerWithAForeignAssetIsIgnored) {
    const auto asset = MakeChainAsset();
    const auto other = MakeChainAsset();
    ASSERT_NE(asset.get(), other.get());

    Playback playback(asset);
    playback.player->asset = other;

    AnimationSystem system;
    // The ignored player never samples its clip: the pose it has never rebuilt
    // falls back to the bind pose, and time does not advance.
    EXPECT_EQ(system.Update(playback.registry, 0.5f), 1u);
    EXPECT_NEAR(playback.player->time, 0.0f, kEps) << "an ignored player does not advance";
    EXPECT_NEAR(WorldX(*playback.pose, 2), 2.0f, kEps) << "bind pose";
    EXPECT_EQ(playback.player->reported_signatures.size(), 1u);

    // Reported once, not per frame.
    EXPECT_EQ(system.Update(playback.registry, 0.5f), 0u);
    EXPECT_EQ(playback.player->reported_signatures.size(), 1u);
}

TEST(AnimationSystem, ClipForAnotherSkeletonIsIgnored) {
    // A second skeleton with a clip retargeted to it, so the pose's skeleton and
    // the clip's skeleton disagree.
    auto asset = std::make_shared<VulkanEngine::SkeletalAsset>(*MakeChainAsset());
    asset->skeletons.push_back(MakeChainSkeleton());
    asset->animations.at(0).skeleton = 1;

    Playback playback(asset);
    EXPECT_EQ(playback.pose->skeleton_index, 0u);

    AnimationSystem system;
    EXPECT_EQ(system.Update(playback.registry, 0.5f), 1u) << "the pose falls back to the bind pose";
    EXPECT_EQ(playback.player->reported_signatures.size(), 1u);
    EXPECT_NEAR(WorldX(*playback.pose, 2), 2.0f, kEps) << "still the bind pose";

    // The same clip played into the skeleton it names works.
    playback.pose->skeleton_index = 1;
    EXPECT_EQ(system.Update(playback.registry, 0.5f), 1u);
    EXPECT_NEAR(WorldX(*playback.pose, 2), 3.0f, kEps);
}

TEST(AnimationSystem, OutOfRangeClipIndexIsIgnored) {
    const auto asset = MakeChainAsset();
    Playback playback(asset);
    playback.player->clip_index = 99;

    AnimationSystem system;
    EXPECT_EQ(system.Update(playback.registry, 0.5f), 1u) << "the pose falls back to the bind pose";
    EXPECT_NEAR(playback.player->time, 0.0f, kEps);
    EXPECT_EQ(playback.player->reported_signatures.size(), 1u);
}

TEST(AnimationSystem, RecoveringFromARejectedPlayerReSamples) {
    const auto asset = MakeChainAsset();
    Playback playback(asset);
    AnimationSystem system;

    ASSERT_EQ(system.Update(playback.registry, 0.5f), 1u);
    EXPECT_NEAR(WorldX(*playback.pose, 2), 3.0f, kEps);
    playback.player->playing = false;

    // Transient inconsistency: the clip index goes out of range while the pose is
    // marked dirty, so the player is rejected and the pose falls back to the bind
    // pose.
    playback.player->clip_index = 99;
    playback.pose->dirty = true;
    ASSERT_EQ(system.Update(playback.registry, 0.0f), 1u);
    EXPECT_NEAR(WorldX(*playback.pose, 2), 2.0f, kEps) << "bind pose";

    // Restoring the clip must re-sample. The rejected update rebuilt the pose, so
    // the player's "already sampled" bookkeeping had to be invalidated; otherwise
    // the same asset/clip/time would match the settle check and the object would
    // stay stuck at the bind pose.
    playback.player->clip_index = 0;
    EXPECT_EQ(system.Update(playback.registry, 0.0f), 1u);
    EXPECT_NEAR(WorldX(*playback.pose, 2), 3.0f, kEps) << "sampled at t = 0.5 again";
}

TEST(AnimationSystem, SwappingAPlayerlessPoseAssetRebuildsIt) {
    const auto asset = MakeChainAsset();
    ComponentRegistry registry{};
    Entity& entity = registry.CreateEntity();
    auto& pose = registry.AddComponent<SkeletonPose>(entity);
    pose.asset = asset;
    pose.skeleton_index = 0;
    pose.dirty = true;

    AnimationSystem system;
    ASSERT_EQ(system.Update(registry, 0.0f), 1u);
    EXPECT_NEAR(WorldX(pose, 2), 2.0f, kEps) << "bind pose: +1 per joint";

    // Re-pointing `asset` is plain field assignment, so nothing sets `dirty`. The
    // pose records what it was built from, which is what detects the swap.
    auto replacement = std::make_shared<VulkanEngine::SkeletalAsset>(*asset);
    replacement->skeletons.at(0).joints.at(1).local_bind = Trs(5.0f, 0.0f, 0.0f);
    pose.asset = replacement;

    EXPECT_EQ(system.Update(registry, 0.0f), 1u);
    EXPECT_NEAR(WorldX(pose, 2), 6.0f, kEps) << "spine at 5 plus joint 2's bind 1";

    // Settled against the new asset: no further rebuilds.
    EXPECT_EQ(system.Update(registry, 0.0f), 0u);
}

TEST(AnimationSystem, ARejectedPlayerStillRebuildsAStalePose) {
    const auto asset = MakeChainAsset();
    Playback playback(asset);
    AnimationSystem system;

    ASSERT_EQ(system.Update(playback.registry, 0.5f), 1u);
    EXPECT_NEAR(WorldX(*playback.pose, 2), 3.0f, kEps);

    // The entity is re-pointed at a replacement asset (a different bind pose) while
    // the player no longer resolves against it. The playerless pass skips
    // player-owned poses, so the reject branch is the only thing that can notice
    // the stale pose.
    auto replacement = std::make_shared<VulkanEngine::SkeletalAsset>(*asset);
    replacement->skeletons.at(0).joints.at(1).local_bind = Trs(5.0f, 0.0f, 0.0f);
    playback.player->asset = replacement;
    playback.pose->asset = replacement;
    playback.player->clip_index = 99;
    playback.player->playing = false;

    EXPECT_EQ(system.Update(playback.registry, 0.0f), 1u);
    EXPECT_NEAR(WorldX(*playback.pose, 2), 6.0f, kEps)
        << "the new asset's bind pose, not the old asset's sampled pose";

    // Settled: the stale-pose trigger fires once.
    EXPECT_EQ(system.Update(playback.registry, 0.0f), 0u);
}

TEST(AnimationSystem, AnExternalPoseRebuildIsSampledOverWhilePaused) {
    const auto asset = MakeChainAsset();
    Playback playback(asset);
    AnimationSystem system;

    ASSERT_EQ(system.Update(playback.registry, 0.5f), 1u);
    EXPECT_NEAR(WorldX(*playback.pose, 2), 3.0f, kEps);

    // The app resets the pose to the rest pose while the player is paused.
    playback.player->playing = false;
    playback.pose->Rebuild();
    EXPECT_NEAR(WorldX(*playback.pose, 2), 2.0f, kEps);
    EXPECT_FALSE(playback.pose->dirty) << "Rebuild clears the flag the settle check reads";

    // The attached player owns the pose, so the next update samples over the reset
    // even though the player's own asset/clip/time did not change.
    EXPECT_EQ(system.Update(playback.registry, 0.0f), 1u);
    EXPECT_NEAR(WorldX(*playback.pose, 2), 3.0f, kEps);
}

TEST(AnimationSystem, SwappingTheAssetOfAPausedPlayerReSamples) {
    const auto asset = MakeChainAsset();
    Playback playback(asset);
    AnimationSystem system;

    ASSERT_EQ(system.Update(playback.registry, 0.5f), 1u);
    EXPECT_NEAR(WorldX(*playback.pose, 2), 3.0f, kEps);

    // A replacement asset with a different bind pose for the un-channelled spine.
    // Every other field of the player (clip, time, paused) stays identical, so only
    // the asset identity can make the pose re-sample.
    auto replacement = std::make_shared<VulkanEngine::SkeletalAsset>(*asset);
    replacement->skeletons.at(0).joints.at(1).local_bind = Trs(5.0f, 0.0f, 0.0f);

    playback.player->playing = false;
    playback.player->asset = replacement;
    playback.pose->asset = replacement;

    EXPECT_EQ(system.Update(playback.registry, 0.0f), 1u);
    // Joint 2 still samples 2.0 at t = 0.5, but the spine now offsets it by 5.
    EXPECT_NEAR(WorldX(*playback.pose, 2), 7.0f, kEps);

    // Settled against the new asset: no further rebuilds.
    EXPECT_EQ(system.Update(playback.registry, 0.0f), 0u);
}

TEST(AnimationSystem, AssetOutlivesItsSourceMeshData) {
    auto data = std::make_unique<MeshData>();
    data->skeletons.push_back(MakeChainSkeleton());
    Skin skin{};
    skin.skeleton = 0;
    skin.joints = {0, 1, 2, 3};
    skin.inverse_bind.assign(4, Identity());
    data->skins.push_back(skin);
    data->animations.push_back(MakeWideClip());

    const auto asset = VulkanEngine::MakeSkeletalAsset(*data);
    // The component pair must not alias registry storage: dropping the source mesh
    // data leaves the asset (and therefore playback) intact.
    data.reset();

    Playback playback(asset);
    AnimationSystem system;
    EXPECT_EQ(system.Update(playback.registry, 0.5f), 1u);
    EXPECT_NEAR(WorldX(*playback.pose, 2), 3.0f, kEps);
    EXPECT_EQ(playback.pose->GetPalette(0).size(), 4u);
}

TEST(AnimationSystem, RebuildFromLocalRejectsAMismatchedLocalArray) {
    const auto asset = MakeChainAsset();
    SkeletonPose pose;
    pose.asset = asset;
    pose.skeleton_index = 0;

    // A caller that sampled into a buffer for a different skeleton must not make
    // the pose read past the end of `local` or compose a partial pose.
    pose.local.resize(1);
    pose.local[0] = Trs(7.0f, 0.0f, 0.0f);
    pose.RebuildFromLocal();

    ASSERT_EQ(pose.world.size(), 4u);
    EXPECT_NEAR(WorldX(pose, 0), 0.0f, kEps) << "bind pose, not the stale partial local";
    EXPECT_NEAR(WorldX(pose, 2), 2.0f, kEps);
}

} // namespace
