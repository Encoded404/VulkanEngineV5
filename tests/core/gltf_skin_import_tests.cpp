#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

import std;

import FileLoader.Types;

import VulkanEngine.Mesh.MeshTypes;
import VulkanEngine.FileLoaders.Mesh.GltfMeshAssembler;

#include "test_gltf_skeleton_fixtures.hpp"

namespace {

using GltfSkeletonFixture::AnimatedGlb;
using GltfSkeletonFixture::InfluenceSetSpec;
using GltfSkeletonFixture::MakeGlb;
using GltfSkeletonFixture::MultiSetGlb;
using GltfSkeletonFixture::SkippedSkinGlb;
using GltfSkeletonFixture::SkinnedGlb;
using GltfSkeletonFixture::TwoSetGlb;
using GltfSkeletonFixture::WriteFixture;
using VulkanEngine::FileLoaders::Mesh::GltfMeshAssembler;
using VulkanEngine::Mesh;
using VulkanEngine::MeshVertexVec3;
using VulkanEngine::SubMesh;
using VulkanEngine::kNoSkin;

class GltfSkinImportTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() / "vkengine_gltf_skin_tests";
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
        std::filesystem::create_directories(dir_);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    static std::shared_ptr<Mesh> Assemble(const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        EXPECT_TRUE(file.is_open());
        const auto size = static_cast<std::size_t>(file.tellg());
        std::vector<std::byte> bytes(size);
        file.seekg(0, std::ios::beg);
        file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
        file.close();

        GltfMeshAssembler assembler;
        assembler.SetSourcePath(path);
        auto buffer = std::make_shared<FileLoader::ByteBuffer>(bytes.begin(), bytes.end());
        return assembler.AssembleFromFullBuffer(std::move(buffer)).get();
    }

    [[nodiscard]] std::shared_ptr<Mesh> AssembleFixture(const std::string& name,
                                                        const std::vector<std::byte>& bytes) {
        const auto path = WriteFixture(dir_, name, bytes);
        return Assemble(path);
    }

    std::filesystem::path dir_{};
};

TEST_F(GltfSkinImportTest, SkeletonSkinsAndWeightsAreImported) {
    const std::shared_ptr<Mesh> mesh = AssembleFixture("skinned.glb", SkinnedGlb());
    ASSERT_NE(mesh, nullptr);

    // Two skeletons: skins 0 and 1 share their ordered joint-node set, skin 2 has
    // a different one.
    EXPECT_EQ(mesh->skins.size(), 3u);
    ASSERT_EQ(mesh->skeletons.size(), 2u);
    EXPECT_EQ(mesh->influence_sets, 1u);
    ASSERT_EQ(mesh->bone_weights.size(), mesh->vertices.size());
    EXPECT_TRUE(mesh->bone_weights_extra.empty());

    // Joint hierarchy: joint0 (node 1) is the root under the translated armature,
    // joint1 (node 2) is its child. Names survive for diagnostics.
    const VulkanEngine::Skeleton& skeleton = mesh->skeletons[0];
    ASSERT_EQ(skeleton.joints.size(), 2u);
    ASSERT_EQ(skeleton.names.size(), 2u);
    EXPECT_EQ(skeleton.joints[0].parent, VulkanEngine::kNoSkeleton);
    EXPECT_EQ(skeleton.joints[1].parent, 0u);
    EXPECT_EQ(skeleton.names[0], "joint0");
    EXPECT_EQ(skeleton.names[1], "joint1");
    ASSERT_EQ(skeleton.roots.size(), 1u);
    EXPECT_EQ(skeleton.roots[0].joint, 0u);
    // The armature's world transform is +10 on X; the joint's own +1 is local.
    EXPECT_NEAR(skeleton.roots[0].transform[12], 10.0f, 1e-5f);
    EXPECT_NEAR(skeleton.joints[0].local_bind.position.x, 1.0f, 1e-5f);

    // Skin 0 maps its two joints to the skeleton's joints, and inverse binds are
    // the (identity) matrix in the file.
    const VulkanEngine::Skin& skin0 = mesh->skins[0];
    EXPECT_EQ(skin0.skeleton, 0u);
    EXPECT_EQ(skin0.joints, (std::vector<std::uint16_t>{0, 1}));
    ASSERT_EQ(skin0.inverse_bind.size(), 2u);
    EXPECT_EQ(skin0.inverse_bind[0], (std::array<float, 16>{1.0f, 0.0f, 0.0f, 0.0f,
                                                            0.0f, 1.0f, 0.0f, 0.0f,
                                                            0.0f, 0.0f, 1.0f, 0.0f,
                                                            0.0f, 0.0f, 0.0f, 1.0f}));

    // Weights stay skin-local: the primitive's JOINTS_0 values are stored raw.
    ASSERT_EQ(mesh->bone_weights.size(), 3u);
    EXPECT_EQ(mesh->bone_weights[0].bone_indices[0], 0u);
    EXPECT_EQ(mesh->bone_weights[0].weights[0], 255u);
    EXPECT_EQ(mesh->bone_weights[1].bone_indices[0], 1u);
    EXPECT_EQ(mesh->bone_weights[1].weights[0], 255u);
    EXPECT_EQ(mesh->bone_weights[2].bone_indices[0], 0u);
    EXPECT_EQ(mesh->bone_weights[2].bone_indices[1], 1u);
    EXPECT_EQ(static_cast<int>(mesh->bone_weights[2].weights[0]) +
                  static_cast<int>(mesh->bone_weights[2].weights[1]),
              255);
}

TEST_F(GltfSkinImportTest, SkinnedPlacementsShareGeometryAndIgnoreTheNodeTransform) {
    const std::shared_ptr<Mesh> mesh = AssembleFixture("placements.glb", SkinnedGlb());
    ASSERT_NE(mesh, nullptr);

    // Three nodes reference one mesh. Geometry is emitted once and shared; only
    // the skin differs per placement.
    ASSERT_EQ(mesh->vertices.size(), 3u);
    ASSERT_EQ(mesh->indices.size(), 3u);
    ASSERT_EQ(mesh->subMeshes.size(), 3u);
    for (const SubMesh& submesh : mesh->subMeshes) {
        EXPECT_EQ(submesh.index_start, 0u);
        EXPECT_EQ(submesh.index_count, 3u);
        // A skinned node's own transform is ignored by glTF; placement is identity.
        EXPECT_NEAR(submesh.transform.position.x, 0.0f, 1e-6f);
        EXPECT_NEAR(submesh.transform.position.y, 0.0f, 1e-6f);
        EXPECT_NEAR(submesh.transform.position.z, 0.0f, 1e-6f);
    }
    EXPECT_EQ(mesh->subMeshes[0].skin_index, 0u);
    EXPECT_EQ(mesh->subMeshes[1].skin_index, 1u);
    EXPECT_EQ(mesh->subMeshes[2].skin_index, 2u);
    EXPECT_EQ(mesh->subMeshes[0].skin_index != kNoSkin, true);
}

TEST_F(GltfSkinImportTest, TwoSkinsOverTheSameJointSetShareOneSkeleton) {
    const std::shared_ptr<Mesh> mesh = AssembleFixture("shared_skeleton.glb", SkinnedGlb());
    ASSERT_NE(mesh, nullptr);

    ASSERT_EQ(mesh->skins.size(), 3u);
    ASSERT_EQ(mesh->skeletons.size(), 2u);
    // Same ordered joint nodes -> one skeleton so one SkeletonPose covers both.
    EXPECT_EQ(mesh->skins[0].skeleton, mesh->skins[1].skeleton);
    // A different joint set remains its own skeleton.
    EXPECT_NE(mesh->skins[2].skeleton, mesh->skins[0].skeleton);
    ASSERT_EQ(mesh->skins[2].joints.size(), 1u);
    EXPECT_EQ(mesh->skins[2].joints[0], 0u);
}

TEST_F(GltfSkinImportTest, DedupKeepsVerticesWithDifferentInfluencesApart) {
    const std::shared_ptr<Mesh> mesh = AssembleFixture("dedup.glb", SkinnedGlb());
    ASSERT_NE(mesh, nullptr);

    // Vertices 0 and 1 share a position and normal but are bound to different
    // joints. They must not be merged, or one corner would follow the wrong bone.
    ASSERT_EQ(mesh->vertices.size(), 3u);
    ASSERT_EQ(mesh->bone_weights.size(), 3u);
    EXPECT_NE(mesh->bone_weights[0].bone_indices[0], mesh->bone_weights[1].bone_indices[0]);
    EXPECT_EQ(mesh->indices[0], 0u);
    EXPECT_EQ(mesh->indices[1], 1u);
    EXPECT_EQ(mesh->indices[2], 2u);
}

TEST_F(GltfSkinImportTest, SecondInfluenceSetImportsAParallelStream) {
    const std::shared_ptr<Mesh> mesh = AssembleFixture("two_sets.glb", TwoSetGlb());
    ASSERT_NE(mesh, nullptr);

    EXPECT_EQ(mesh->influence_sets, 2u);
    ASSERT_EQ(mesh->bone_weights.size(), mesh->vertices.size());
    ASSERT_EQ(mesh->bone_weights_extra.size(), mesh->vertices.size());

    // Both sets are read and normalized together: 0.8 from JOINTS_0/WEIGHTS_0 plus
    // 0.2 from JOINTS_1/WEIGHTS_1 keeps 204 and 51, so the two records sum to 255.
    // The engine packs a vertex's influences into eight slots (base first, then the
    // extra record), not one record per authored set, so both live in the base
    // record here and the extra record is all zero.
    for (std::size_t i = 0; i < mesh->vertices.size(); ++i) {
        EXPECT_EQ(mesh->bone_weights[i].bone_indices[0], 0u);
        EXPECT_EQ(mesh->bone_weights[i].weights[0], 204u);
        EXPECT_EQ(mesh->bone_weights[i].bone_indices[1], 1u) << "the second set's joint";
        EXPECT_EQ(mesh->bone_weights[i].weights[1], 51u);
        for (std::size_t k = 0; k < 4; ++k) {
            EXPECT_EQ(mesh->bone_weights_extra[i].weights[k], 0u);
            EXPECT_EQ(mesh->bone_weights_extra[i].bone_indices[k], 0u);
        }
        int total = 0;
        for (std::size_t k = 0; k < 4; ++k) {
            total += static_cast<int>(mesh->bone_weights[i].weights[k]) +
                     static_cast<int>(mesh->bone_weights_extra[i].weights[k]);
        }
        EXPECT_EQ(total, 255);
    }
}

TEST_F(GltfSkinImportTest, FiveInfluencesFillBothRecords) {
    // Three influences in set 0 and two in set 1: the fifth has to land in the
    // extra record, which is what makes `influence_sets == 2` load bearing.
    InfluenceSetSpec base_set{};
    base_set.joints = {0, 1, 2, 0};
    base_set.weights = {100, 60, 40, 0};
    InfluenceSetSpec extra_set{};
    extra_set.joints = {1, 0, 0, 0};
    extra_set.weights = {40, 15, 0, 0};

    const std::shared_ptr<Mesh> mesh = AssembleFixture("five.glb", MultiSetGlb({base_set, extra_set}));
    ASSERT_NE(mesh, nullptr);
    ASSERT_EQ(mesh->influence_sets, 2u);
    ASSERT_EQ(mesh->bone_weights_extra.size(), mesh->vertices.size());

    // Slot order is the author order of the non-zero influences (set 0 first, then
    // set 1), so the first four fill the base record and the fifth the extra one.
    // 100 + 60 + 40 + 40 + 15 already sums to 255, so the bytes pass through.
    const auto& b = mesh->bone_weights.front();
    const auto& e = mesh->bone_weights_extra.front();
    std::vector<std::pair<unsigned, unsigned>> pairs; // (weight, joint)
    int total = 0;
    for (std::size_t k = 0; k < 4; ++k) {
        if (b.weights[k] != 0) pairs.emplace_back(b.weights[k], b.bone_indices[k]);
        if (e.weights[k] != 0) pairs.emplace_back(e.weights[k], e.bone_indices[k]);
        total += static_cast<int>(b.weights[k]) + static_cast<int>(e.weights[k]);
    }
    EXPECT_EQ(total, 255);
    EXPECT_EQ(pairs.size(), 5u) << "every authored influence survives";
    EXPECT_NE(std::find(pairs.begin(), pairs.end(), std::pair{40u, 1u}), pairs.end())
        << "set 1's 40-weight influence keeps its joint index";
    EXPECT_NE(std::find(pairs.begin(), pairs.end(), std::pair{15u, 0u}), pairs.end());
    EXPECT_NE(std::find(pairs.begin(), pairs.end(), std::pair{100u, 0u}), pairs.end());
    EXPECT_NE(std::find(pairs.begin(), pairs.end(), std::pair{60u, 1u}), pairs.end());
    EXPECT_NE(std::find(pairs.begin(), pairs.end(), std::pair{40u, 2u}), pairs.end());
    // At least one influence had to go in the extra record.
    EXPECT_GT(e.weights[0] + e.weights[1] + e.weights[2] + e.weights[3], 0u);
}

TEST_F(GltfSkinImportTest, MoreThanEightInfluencesKeepTheHeaviest) {
    // Nine non-zero influences across three sets. The single influence in set 2 is
    // the heaviest, so keeping the first eight by set order would lose it; the
    // importer has to select by weight.
    InfluenceSetSpec set0{};
    set0.joints = {0, 1, 2, 0};
    set0.weights = {200, 10, 10, 10};
    InfluenceSetSpec set1{};
    set1.joints = {1, 0, 0, 0};
    set1.weights = {10, 10, 10, 10};
    InfluenceSetSpec set2{};
    set2.joints = {1, 0, 0, 0};
    set2.weights = {200, 0, 0, 0};

    const std::shared_ptr<Mesh> mesh = AssembleFixture("nine.glb", MultiSetGlb({set0, set1, set2}));
    ASSERT_NE(mesh, nullptr);
    ASSERT_EQ(mesh->influence_sets, 2u);
    ASSERT_EQ(mesh->bone_weights_extra.size(), mesh->vertices.size());

    const auto& b = mesh->bone_weights.front();
    const auto& e = mesh->bone_weights_extra.front();
    std::array<std::uint8_t, 8> weights{};
    std::array<std::uint8_t, 8> joints{};
    for (std::size_t k = 0; k < 4; ++k) {
        weights[k] = b.weights[k];
        joints[k] = b.bone_indices[k];
        weights[4 + k] = e.weights[k];
        joints[4 + k] = e.bone_indices[k];
    }

    int total = 0;
    int nonzero = 0;
    int heaviest = 0;
    for (const std::uint8_t w : weights) {
        total += w;
        if (w > 0) ++nonzero;
        heaviest = std::max<int>(heaviest, w);
    }
    EXPECT_EQ(total, 255);
    EXPECT_EQ(nonzero, 8) << "the ninth (smallest) influence is dropped";

    // The two 200-weight influences survive and stay the largest: one from set 0
    // and the one from set 2, which set order alone would have discarded. They hold
    // 400 of the kept 460, so each quantizes to about 111.
    EXPECT_EQ(joints[0], 0u);
    EXPECT_EQ(joints[1], 1u) << "set 2's heavy influence, kept despite being the ninth";
    EXPECT_GE(weights[0], 100u);
    EXPECT_GE(weights[1], 100u);
    EXPECT_LE(std::abs(static_cast<int>(weights[0]) - static_cast<int>(weights[1])), 1);
    // The remaining six are the small ones, renormalized from 10/460 each.
    for (std::size_t k = 2; k < 8; ++k) {
        EXPECT_GT(weights[k], 0u) << "a kept influence must not quantize away";
        EXPECT_LE(weights[k], 7u);
    }
}

TEST_F(GltfSkinImportTest, UnskinnedPrimitiveBesideASkinnedOneGetsZeroWeights) {
    // Two glTF meshes over the same accessors: mesh 0 is unskinned, mesh 1 is
    // skinned. The flat engine Mesh must keep the weight stream parallel for
    // every vertex, backfilling the already-emitted unskinned ones with zeros.
    const std::string json = std::string(R"json({
  "asset": {"version": "2.0"},
  "scene": 0,
  "scenes": [{"nodes": [0, 3, 4]}],
  "nodes": [
    {"name": "rig", "children": [1, 2]},
    {"name": "joint0"},
    {"name": "joint1"},
    {"mesh": 0},
    {"mesh": 1, "skin": 0}
  ],
  "skins": [{"joints": [1, 2], "inverseBindMatrices": 5}],
  "meshes": [
    {"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1}, "indices": 4, "mode": 4}]},
    {"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "JOINTS_0": 2, "WEIGHTS_0": 3}, "indices": 4, "mode": 4}]}
  ],
  "buffers": [{"byteLength": )json") + std::to_string(GltfSkeletonFixture::kGeometryEnd) + R"json(}],
  "bufferViews": [)json" + GltfSkeletonFixture::kGeometryBufferViews + R"json(],
  "accessors": [)json" + GltfSkeletonFixture::kGeometryAccessors + R"json(]
})json";
    // Node 3 and node 4 place the two meshes; node 4 is the skinned one.
    const auto path = WriteFixture(dir_, "mixed.glb", MakeGlb(json, GltfSkeletonFixture::GeometryBin()));
    const std::shared_ptr<Mesh> mesh = Assemble(path);
    ASSERT_NE(mesh, nullptr);

    ASSERT_EQ(mesh->influence_sets, 1u);
    // The unskinned primitive merges its two co-located corners (2 vertices);
    // the skinned primitive keeps them apart because their joints differ (3).
    ASSERT_EQ(mesh->vertices.size(), 5u);
    ASSERT_EQ(mesh->bone_weights.size(), 5u);
    for (std::size_t i = 0; i < 2; ++i) {
        EXPECT_EQ(mesh->bone_weights[i].weights[0], 0u);
        EXPECT_EQ(mesh->bone_weights[i].bone_indices[0], 0u);
    }
    for (std::size_t i = 2; i < mesh->bone_weights.size(); ++i) {
        EXPECT_GT(mesh->bone_weights[i].weights[0], 0u);
    }
    EXPECT_EQ(mesh->bone_weights[2].bone_indices[0], 0u);
}

TEST_F(GltfSkinImportTest, SkippedSkinKeepsTheNodePlacementAndStaysUnskinned) {
    // The only skin has one valid joint and one missing node. The positional joint
    // list cannot be shortened safely, so the whole skin is skipped. The node must
    // keep its own transform rather than being identity-placed as if it resolved.
    const std::shared_ptr<Mesh> mesh = AssembleFixture("skipped_skin.glb", SkippedSkinGlb());
    ASSERT_NE(mesh, nullptr);

    ASSERT_EQ(mesh->skins.size(), 1u);
    EXPECT_EQ(mesh->skins[0].skeleton, VulkanEngine::kNoSkeleton);
    EXPECT_TRUE(mesh->skins[0].joints.empty());
    ASSERT_EQ(mesh->subMeshes.size(), 1u);
    EXPECT_EQ(mesh->subMeshes[0].skin_index, kNoSkin);
    EXPECT_NEAR(mesh->subMeshes[0].transform.position.x, 1.0f, 1e-5f);
    EXPECT_NEAR(mesh->subMeshes[0].transform.position.y, 2.0f, 1e-5f);
    EXPECT_NEAR(mesh->subMeshes[0].transform.position.z, 3.0f, 1e-5f);
}

// The animation fixture also exercises a second skeleton and skinned placement;
// import must not require clips to resolve a skin.
TEST_F(GltfSkinImportTest, AnimatedAssetStillImportsItsSkin) {
    const std::shared_ptr<Mesh> mesh = AssembleFixture("animated_skin.glb", AnimatedGlb());
    ASSERT_NE(mesh, nullptr);
    EXPECT_EQ(mesh->influence_sets, 1u);
    ASSERT_EQ(mesh->subMeshes.size(), 1u);
    EXPECT_EQ(mesh->subMeshes[0].skin_index, 0u);
    ASSERT_EQ(mesh->skins.size(), 2u);
    ASSERT_EQ(mesh->skeletons.size(), 2u);
}

} // namespace
