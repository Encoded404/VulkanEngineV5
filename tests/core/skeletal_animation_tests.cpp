#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <span>
#include <vector>

import std;

import VulkanEngine.Mesh.MeshTypes;
import VulkanEngine.Mesh.SkeletalAnimation;
import VulkanEngine.GpuResources.MeshData;
import VulkanEngine.Components.SkeletonPose;

namespace {

using VulkanEngine::AnimationChannel;
using VulkanEngine::AnimationClip;
using VulkanEngine::AnimationInterpolation;
using VulkanEngine::AnimationKey;
using VulkanEngine::AnimationPath;
using VulkanEngine::MeshVertexVec3;
using VulkanEngine::Skeleton;
using VulkanEngine::SkeletonJoint;
using VulkanEngine::SkeletonRoot;
using VulkanEngine::Skin;
using VulkanEngine::SubMeshTransform;
using VulkanEngine::kNoSkeleton;

using Mat4 = std::array<float, 16>;

constexpr float kEps = 1e-5f;

[[nodiscard]] constexpr Mat4 Identity() {
    return {1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f};
}

[[nodiscard]] constexpr Mat4 Translation(const float x, const float y, const float z) {
    Mat4 m = Identity();
    m[12] = x;
    m[13] = y;
    m[14] = z;
    return m;
}

// Column-major product.
[[nodiscard]] Mat4 Multiply(const Mat4& a, const Mat4& b) {
    Mat4 out{};
    for (std::size_t c = 0; c < 4; ++c) {
        for (std::size_t r = 0; r < 4; ++r) {
            float sum = 0.0f;
            for (std::size_t k = 0; k < 4; ++k) sum += a[k * 4 + r] * b[c * 4 + k];
            out[c * 4 + r] = sum;
        }
    }
    return out;
}

// The column-vector interpretation of a column-major matrix: M * p.
[[nodiscard]] MeshVertexVec3 TransformPoint(const Mat4& m, const MeshVertexVec3& p) {
    return MeshVertexVec3{
        m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12],
        m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
        m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14],
    };
}

// The shader-side read: Slang interprets the 16 floats as a row-major matrix S,
// and `mul(float4(p, 1), S)` computes p^T * S. Reading the same bytes that way
// must transform a point exactly as the column-major matrix does.
[[nodiscard]] MeshVertexVec3 ShaderMulPoint(const Mat4& palette, const MeshVertexVec3& p) {
    return MeshVertexVec3{
        p.x * palette[0] + p.y * palette[4] + p.z * palette[8] + palette[12],
        p.x * palette[1] + p.y * palette[5] + p.z * palette[9] + palette[13],
        p.x * palette[2] + p.y * palette[6] + p.z * palette[10] + palette[14],
    };
}

[[nodiscard]] SubMeshTransform Trs(const float x, const float y, const float z) {
    SubMeshTransform t{};
    t.position = MeshVertexVec3{x, y, z};
    return t;
}

void ExpectPoint(const MeshVertexVec3& actual, const float x, const float y, const float z) {
    EXPECT_NEAR(actual.x, x, kEps);
    EXPECT_NEAR(actual.y, y, kEps);
    EXPECT_NEAR(actual.z, z, kEps);
}

[[nodiscard]] Skeleton SingleJointSkeleton(const SubMeshTransform& bind, const Mat4& root = Identity()) {
    Skeleton skeleton;
    skeleton.joints.resize(1);
    skeleton.joints[0].local_bind = bind;
    skeleton.joints[0].parent = kNoSkeleton;
    skeleton.roots.push_back(SkeletonRoot{.joint = 0, .transform = root});
    return skeleton;
}

[[nodiscard]] std::vector<SubMeshTransform> LocalFrom(const Skeleton& skeleton) {
    std::vector<SubMeshTransform> local(skeleton.joints.size());
    for (std::size_t i = 0; i < skeleton.joints.size(); ++i) {
        local[i] = skeleton.joints[i].local_bind;
    }
    return local;
}

// ── hierarchy and bind pose ─────────────────────────────────────────────────

TEST(SkeletalAnimation, ComputeWorldIsOrderIndependent) {
    // joints[0] is a child stored *before* its parent at joints[1]; only the
    // parent indices may drive composition.
    Skeleton skeleton;
    skeleton.joints.resize(2);
    skeleton.joints[0].parent = 1;
    skeleton.joints[0].local_bind = Trs(0.0f, 1.0f, 0.0f);
    skeleton.joints[1].parent = kNoSkeleton;
    skeleton.joints[1].local_bind = Trs(2.0f, 0.0f, 0.0f);
    skeleton.roots.push_back(SkeletonRoot{.joint = 1, .transform = Identity()});

    const auto local = LocalFrom(skeleton);
    std::vector<Mat4> world(2);
    VulkanEngine::ComputeWorld(skeleton, local, world);

    ExpectPoint(TransformPoint(world[1], MeshVertexVec3{}), 2.0f, 0.0f, 0.0f);
    ExpectPoint(TransformPoint(world[0], MeshVertexVec3{}), 2.0f, 1.0f, 0.0f);
}

TEST(SkeletalAnimation, ArmatureRootTransformStartsThePose) {
    // A non-identity armature above the root joint must move the whole pose;
    // glTF inverse-bind matrices are expressed below the armature node.
    const Skeleton skeleton = SingleJointSkeleton(Trs(1.0f, 0.0f, 0.0f), Translation(10.0f, 0.0f, 0.0f));

    const auto local = LocalFrom(skeleton);
    std::vector<Mat4> world(1);
    VulkanEngine::ComputeWorld(skeleton, local, world);
    ExpectPoint(TransformPoint(world[0], MeshVertexVec3{}), 11.0f, 0.0f, 0.0f);

    std::vector<Mat4> bind(1);
    VulkanEngine::ResolveBindPose(skeleton, bind);
    ExpectPoint(TransformPoint(bind[0], MeshVertexVec3{}), 11.0f, 0.0f, 0.0f);
}

TEST(SkeletalAnimation, FromTrsComposesTranslationRotationAndScale) {
    SubMeshTransform trs{};
    trs.position = MeshVertexVec3{1.0f, 2.0f, 3.0f};
    trs.rotation[2] = std::sin(0.785398163f); // 45 degrees about Z
    trs.rotation[3] = std::cos(0.785398163f);
    trs.scale = MeshVertexVec3{2.0f, 2.0f, 2.0f};

    const Mat4 m = VulkanEngine::FromTRS(trs);
    // The stored quaternion is a half-angle, so this is a 90 degree rotation
    // about Z. Local (1,0,0) scales to (2,0,0), rotates to (0,2,0), then moves
    // by (1,2,3): scale is applied first, translation last.
    ExpectPoint(TransformPoint(m, MeshVertexVec3{1.0f, 0.0f, 0.0f}), 1.0f, 4.0f, 3.0f);
}

// ── palette and the matrix convention ──────────────────────────────────────

TEST(SkeletalAnimation, PaletteIsWorldTimesInverseBind) {
    const Skeleton skeleton = SingleJointSkeleton(Trs(0.0f, 0.0f, 0.0f));
    const Mat4 world = Translation(2.0f, 3.0f, 4.0f);

    Skin skin{};
    skin.skeleton = 0;
    skin.joints = {0};
    skin.inverse_bind = {Translation(-1.0f, -1.0f, -1.0f)};

    std::vector<Mat4> palette(1);
    VulkanEngine::ComputePalette(skin, std::span<const Mat4>(&world, 1), palette);

    const Mat4 expected = Multiply(world, skin.inverse_bind[0]);
    const MeshVertexVec3 point{5.0f, 6.0f, 7.0f};
    ExpectPoint(TransformPoint(expected, point), 6.0f, 8.0f, 10.0f);
    EXPECT_EQ(palette[0], expected) << "ComputePalette must store the column-major product bytes";
}

TEST(SkeletalAnimation, PaletteTransformsLikeTheRowVectorShader) {
    // Build M = world * inverse_bind, store transpose(M) in the row-major sense
    // (which is the same byte sequence as column-major M), and compare the
    // shader's `mul(float4(p, 1), m)` with the CPU column-vector reference.
    const Mat4 world = Translation(1.5f, -2.0f, 0.25f);
    const Mat4 inverse_bind = Translation(-0.5f, 0.5f, 1.0f);
    const Mat4 m = Multiply(world, inverse_bind);

    Skin skin{};
    skin.joints = {0};
    skin.inverse_bind = {inverse_bind};
    std::vector<Mat4> palette(1);
    VulkanEngine::ComputePalette(skin, std::span<const Mat4>(&world, 1), palette);

    for (const MeshVertexVec3 p : {MeshVertexVec3{0.0f, 0.0f, 0.0f},
                                   MeshVertexVec3{1.0f, 2.0f, 3.0f},
                                   MeshVertexVec3{-4.0f, 0.5f, 2.0f}}) {
        const MeshVertexVec3 cpu = TransformPoint(m, p);
        const MeshVertexVec3 shader = ShaderMulPoint(palette[0], p);
        ExpectPoint(shader, cpu.x, cpu.y, cpu.z);
    }
}

TEST(SkeletalAnimation, PaletteToleratesOutOfRangeSkinJoints) {
    Skin skin{};
    skin.joints = {7};
    skin.inverse_bind = {Translation(1.0f, 2.0f, 3.0f)};
    const Mat4 world = Identity();
    std::vector<Mat4> palette(1);
    VulkanEngine::ComputePalette(skin, std::span<const Mat4>(&world, 1), palette);
    EXPECT_EQ(palette[0], Identity()) << "an out-of-range joint must not read past world";
}

// ── clip sampling ──────────────────────────────────────────────────────────

TEST(SkeletalAnimation, ClipSamplingEndpointsAndWrap) {
    Skeleton skeleton = SingleJointSkeleton(Trs(0.0f, 0.0f, 0.0f));

    AnimationClip clip{};
    clip.name = "move";
    clip.skeleton = 0;
    clip.duration = 2.0f;
    AnimationChannel channel{};
    channel.joint = 0;
    channel.path = AnimationPath::Translation;
    channel.interpolation = AnimationInterpolation::Linear;
    channel.keys = {
        AnimationKey{.time = 0.0f, .value = {0.0f, 0.0f, 0.0f, 0.0f}},
        AnimationKey{.time = 2.0f, .value = {2.0f, 0.0f, 0.0f, 0.0f}},
    };
    clip.channels.push_back(channel);

    std::vector<SubMeshTransform> local(1);
    VulkanEngine::SampleClip(clip, skeleton, 0.0f, local);
    ExpectPoint(local[0].position, 0.0f, 0.0f, 0.0f);

    VulkanEngine::SampleClip(clip, skeleton, 1.0f, local);
    ExpectPoint(local[0].position, 1.0f, 0.0f, 0.0f);

    // 3 wraps to 1 at a 2 second duration.
    VulkanEngine::SampleClip(clip, skeleton, 3.0f, local);
    ExpectPoint(local[0].position, 1.0f, 0.0f, 0.0f);

    // A negative time wraps forward.
    VulkanEngine::SampleClip(clip, skeleton, -0.5f, local);
    ExpectPoint(local[0].position, 1.5f, 0.0f, 0.0f);
}

TEST(SkeletalAnimation, StepDoesNotInterpolate) {
    Skeleton skeleton = SingleJointSkeleton(Trs(0.0f, 0.0f, 0.0f));

    AnimationClip clip{};
    clip.duration = 1.0f;
    AnimationChannel channel{};
    channel.joint = 0;
    channel.path = AnimationPath::Translation;
    channel.keys = {
        AnimationKey{.time = 0.0f, .value = {0.0f, 0.0f, 0.0f, 0.0f}},
        AnimationKey{.time = 1.0f, .value = {10.0f, 0.0f, 0.0f, 0.0f}},
    };

    std::vector<SubMeshTransform> local(1);
    channel.interpolation = AnimationInterpolation::Step;
    clip.channels = {channel};
    VulkanEngine::SampleClip(clip, skeleton, 0.5f, local);
    ExpectPoint(local[0].position, 0.0f, 0.0f, 0.0f);

    channel.interpolation = AnimationInterpolation::Linear;
    clip.channels = {channel};
    VulkanEngine::SampleClip(clip, skeleton, 0.5f, local);
    ExpectPoint(local[0].position, 5.0f, 0.0f, 0.0f);
}

TEST(SkeletalAnimation, RotationUsesShortestArcSlerp) {
    Skeleton skeleton = SingleJointSkeleton(Trs(0.0f, 0.0f, 0.0f));

    AnimationClip clip{};
    clip.duration = 1.0f;
    AnimationChannel channel{};
    channel.joint = 0;
    channel.path = AnimationPath::Rotation;
    channel.interpolation = AnimationInterpolation::Linear;
    channel.keys = {
        AnimationKey{.time = 0.0f, .value = {0.0f, 0.0f, 0.0f, 1.0f}},
        AnimationKey{.time = 1.0f, .value = {0.0f, 0.0f, 0.70710678f, 0.70710678f}}, // 90 deg about Z
    };
    clip.channels.push_back(channel);

    std::vector<SubMeshTransform> local(1);
    VulkanEngine::SampleClip(clip, skeleton, 0.25f, local);

    // Slerp at 25% of a 90 degree arc is 22.5 degrees. A componentwise lerp
    // would give roughly 21.6 degrees; the tolerance is tighter than that gap.
    constexpr float kHalfAngle = 11.25f * 3.14159265358979f / 180.0f;
    EXPECT_NEAR(local[0].rotation[2], std::sin(kHalfAngle), 1e-4f);
    EXPECT_NEAR(local[0].rotation[3], std::cos(kHalfAngle), 1e-4f);
    EXPECT_NEAR(local[0].rotation[0], 0.0f, 1e-6f);
    EXPECT_NEAR(local[0].rotation[1], 0.0f, 1e-6f);
    const float length = std::sqrt(local[0].rotation[0] * local[0].rotation[0] +
                                   local[0].rotation[1] * local[0].rotation[1] +
                                   local[0].rotation[2] * local[0].rotation[2] +
                                   local[0].rotation[3] * local[0].rotation[3]);
    EXPECT_NEAR(length, 1.0f, 1e-5f);
}

TEST(SkeletalAnimation, SampleClipRestoresBindPoseForUnanimatedJoints) {
    Skeleton skeleton;
    skeleton.joints.resize(2);
    skeleton.joints[0].parent = kNoSkeleton;
    skeleton.joints[0].local_bind = Trs(0.0f, 0.0f, 0.0f);
    skeleton.joints[1].parent = 0;
    skeleton.joints[1].local_bind = Trs(5.0f, 0.0f, 0.0f);
    skeleton.roots.push_back(SkeletonRoot{.joint = 0, .transform = Identity()});

    AnimationClip clip{};
    clip.duration = 1.0f;
    AnimationChannel channel{};
    channel.joint = 0;
    channel.path = AnimationPath::Translation;
    channel.keys = {
        AnimationKey{.time = 0.0f, .value = {1.0f, 0.0f, 0.0f, 0.0f}},
        AnimationKey{.time = 1.0f, .value = {3.0f, 0.0f, 0.0f, 0.0f}},
    };
    clip.channels.push_back(channel);

    std::vector<SubMeshTransform> local = {Trs(0.0f, 0.0f, 0.0f), Trs(-1.0f, -1.0f, -1.0f)};
    VulkanEngine::SampleClip(clip, skeleton, 0.5f, local);
    ExpectPoint(local[0].position, 2.0f, 0.0f, 0.0f);
    // joint 1 is not channelled by this clip, so it returns to its bind pose.
    ExpectPoint(local[1].position, 5.0f, 0.0f, 0.0f);
}

// ── weight quantization ────────────────────────────────────────────────────

TEST(SkeletalAnimation, ZeroWeightsAreRejected) {
    std::array<float, 8> weights{};
    EXPECT_FALSE(VulkanEngine::NormalizeAndQuantizeWeights(weights));
    for (const float weight : weights) EXPECT_EQ(weight, 0.0f);
}

TEST(SkeletalAnimation, WeightsNormalizeAndQuantizeToExactly255) {
    std::array<float, 8> weights{1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f, 0.0f,
                                 0.0f, 0.0f, 0.0f, 0.0f};
    ASSERT_TRUE(VulkanEngine::NormalizeAndQuantizeWeights(weights));
    int total = 0;
    for (const float weight : weights) {
        total += static_cast<int>(std::lround(weight * 255.0f));
    }
    EXPECT_EQ(total, 255);
}

TEST(SkeletalAnimation, TinyWeightsAreDropped) {
    std::array<float, 8> weights{1.0f, 1e-6f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    ASSERT_TRUE(VulkanEngine::NormalizeAndQuantizeWeights(weights));
    EXPECT_EQ(std::lround(weights[0] * 255.0f), 255);
    EXPECT_EQ(std::lround(weights[1] * 255.0f), 0);
}

// ── the pose component and asset ownership ─────────────────────────────────

TEST(SkeletonPose, MultiSkeletonAssetLeavesTheOtherPosePaletteEmpty) {
    VulkanEngine::GpuResources::MeshData data;
    data.skeletons.resize(2);
    data.skeletons[0] = SingleJointSkeleton(Trs(1.0f, 0.0f, 0.0f));
    data.skeletons[1] = SingleJointSkeleton(Trs(0.0f, 2.0f, 0.0f));

    Skin skin0{};
    skin0.skeleton = 0;
    skin0.joints = {0};
    skin0.inverse_bind = {Identity()};
    Skin skin1{};
    skin1.skeleton = 1;
    skin1.joints = {0};
    skin1.inverse_bind = {Identity()};
    data.skins = {skin0, skin1};

    const auto asset = VulkanEngine::MakeSkeletalAsset(data);

    VulkanEngine::Components::SkeletonPose pose;
    pose.asset = asset;
    pose.skeleton_index = 0;
    pose.Rebuild();

    EXPECT_FALSE(pose.dirty);
    EXPECT_EQ(pose.version, 1u);
    ASSERT_EQ(pose.world.size(), 1u);
    ExpectPoint(TransformPoint(pose.world[0], MeshVertexVec3{}), 1.0f, 0.0f, 0.0f);
    ASSERT_EQ(pose.palettes.size(), 2u);
    EXPECT_FALSE(pose.GetPalette(0).empty());
    EXPECT_TRUE(pose.GetPalette(1).empty()) << "skin 1 belongs to the other skeleton";
    EXPECT_TRUE(pose.GetPalette(99).empty());

    pose.Rebuild();
    EXPECT_EQ(pose.version, 2u);
}

TEST(SkeletonPose, AssetOutlivesItsSourceMeshData) {
    auto data = std::make_unique<VulkanEngine::GpuResources::MeshData>();
    data->skeletons.resize(1);
    data->skeletons[0] = SingleJointSkeleton(Trs(3.0f, 0.0f, 0.0f));
    data->skins.resize(1);
    data->skins[0].skeleton = 0;
    data->skins[0].joints = {0};
    data->skins[0].inverse_bind = {Identity()};

    const auto asset = VulkanEngine::MakeSkeletalAsset(*data);
    data.reset();

    ASSERT_EQ(asset->skeletons.size(), 1u);
    ASSERT_EQ(asset->skins.size(), 1u);

    VulkanEngine::Components::SkeletonPose pose;
    pose.asset = asset;
    pose.skeleton_index = 0;
    pose.Rebuild();
    ASSERT_EQ(pose.world.size(), 1u);
    ExpectPoint(TransformPoint(pose.world[0], MeshVertexVec3{}), 3.0f, 0.0f, 0.0f);
}

TEST(SkeletonPose, RebuildWithoutAnAssetIsEmpty) {
    VulkanEngine::Components::SkeletonPose pose;
    pose.Rebuild();
    EXPECT_TRUE(pose.world.empty());
    EXPECT_TRUE(pose.GetPalette(0).empty());
    EXPECT_FALSE(pose.dirty);
}

} // namespace
