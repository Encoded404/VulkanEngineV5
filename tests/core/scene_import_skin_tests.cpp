#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

import std;

import VulkanEngine.SceneLoader;
import VulkanEngine.Mesh.MeshTypes;
import VulkanEngine.GpuResources.MeshData;

#include "test_gltf_skeleton_fixtures.hpp"

namespace {

using GltfSkeletonFixture::AnimatedGlb;
using GltfSkeletonFixture::WriteFixture;
using VulkanEngine::SceneLoader::LoadedMeshData;
using VulkanEngine::SceneLoader::LoadAllMeshes;
using VulkanEngine::SceneLoader::LoadMeshData;
using VulkanEngine::SceneLoader::LoadMeshFromFile;
using VulkanEngine::SceneLoader::LoadMeshFromFilePath;
using VulkanEngine::SceneLoader::ToMeshData;

// The skeletal copy-site guard: every Mesh -> LoadedMeshData entry point and
// ToMeshData must carry weights, influence sets, skeletons, skins and clips.
// These tests deliberately go through SceneLoader, not the glTF assembler, so a
// dropped copy shows up here.
class SceneImportSkinTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() / "vkengine_scene_import_skin_tests";
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
        std::filesystem::create_directories(dir_);
        (void)WriteFixture(dir_, "rigged.glb", AnimatedGlb());
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    std::filesystem::path dir_{};
};

void ExpectSkeletalData(const LoadedMeshData& loaded) {
    ASSERT_EQ(loaded.submeshes.size(), 1u) << "fixture fell back to the fallback quad";
    EXPECT_EQ(loaded.submeshes[0].skin_index, 0u);
    EXPECT_EQ(loaded.influence_sets, 1u);
    ASSERT_EQ(loaded.bone_weights.size(), loaded.positions.size() / 3U);
    EXPECT_TRUE(loaded.bone_weights_extra.empty());
    ASSERT_EQ(loaded.skeletons.size(), 2u);
    ASSERT_EQ(loaded.skins.size(), 2u);
    ASSERT_EQ(loaded.animations.size(), 4u);
    EXPECT_EQ(loaded.skins[0].skeleton, 0u);
    EXPECT_EQ(loaded.skins[1].skeleton, 1u);
    EXPECT_EQ(loaded.skeletons[0].joints.size(), 2u);
    EXPECT_FALSE(loaded.skeletons[0].roots.empty());
    EXPECT_EQ(loaded.animations[0].channels.size(), 2u);
}

void ExpectSkeletalMeshData(const VulkanEngine::GpuResources::MeshData& data) {
    ASSERT_EQ(data.sub_meshes.size(), 1u);
    EXPECT_EQ(data.sub_meshes[0].skin_index, 0u);
    EXPECT_EQ(data.influence_sets, 1u);
    ASSERT_EQ(data.bone_weights.size(), data.vertices.size());
    EXPECT_TRUE(data.bone_weights_extra.empty());
    EXPECT_EQ(data.skeletons.size(), 2u);
    EXPECT_EQ(data.skins.size(), 2u);
    EXPECT_EQ(data.animations.size(), 4u);
}

TEST_F(SceneImportSkinTest, SingleFileEntryPointCarriesSkeletalData) {
    const LoadedMeshData loaded = LoadMeshFromFilePath(dir_ / "rigged.glb", nullptr);
    ExpectSkeletalData(loaded);

    const auto data = ToMeshData(loaded);
    ExpectSkeletalMeshData(data);
}

TEST_F(SceneImportSkinTest, LoadMeshDataCarriesSkeletalData) {
    const auto data = LoadMeshData(dir_ / "rigged.glb", nullptr);
    ExpectSkeletalMeshData(data);
}

TEST_F(SceneImportSkinTest, DirectoryAndMultiFileEntryPointsCarrySkeletalData) {
    const LoadedMeshData from_dir = LoadMeshFromFile(dir_, nullptr);
    ExpectSkeletalData(from_dir);

    std::vector<LoadedMeshData> meshes;
    std::vector<std::string> names;
    ASSERT_TRUE(LoadAllMeshes(dir_, meshes, names, nullptr));
    ASSERT_EQ(meshes.size(), 1u);
    ExpectSkeletalData(meshes[0]);

    const auto data = ToMeshData(meshes[0]);
    ExpectSkeletalMeshData(data);
}

} // namespace
