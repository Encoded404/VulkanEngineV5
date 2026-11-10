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
using GltfSkeletonFixture::OverlappingSkeletonGlb;
using GltfSkeletonFixture::WriteFixture;
using VulkanEngine::AnimationClip;
using VulkanEngine::AnimationInterpolation;
using VulkanEngine::AnimationPath;
using VulkanEngine::FileLoaders::Mesh::GltfMeshAssembler;
using VulkanEngine::Mesh;

class GltfAnimationImportTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() / "vkengine_gltf_animation_tests";
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

    [[nodiscard]] std::shared_ptr<Mesh> AssembleAnimated() {
        const auto path = WriteFixture(dir_, "animated.glb", AnimatedGlb());
        return Assemble(path);
    }

    static const AnimationClip* FindClip(const Mesh& mesh, const std::string& name) {
        for (const AnimationClip& clip : mesh.animations) {
            if (clip.name == name) return &clip;
        }
        return nullptr;
    }

    std::filesystem::path dir_{};
};

TEST_F(GltfAnimationImportTest, ClipsMapChannelsToSkeletonJoints) {
    const std::shared_ptr<Mesh> mesh = AssembleAnimated();
    ASSERT_NE(mesh, nullptr);

    // walk, cubic, and the two halves of the split multi-skeleton animation.
    ASSERT_EQ(mesh->animations.size(), 4u);

    const AnimationClip* walk = FindClip(*mesh, "walk");
    ASSERT_NE(walk, nullptr);
    EXPECT_EQ(walk->skeleton, 0u);
    EXPECT_NEAR(walk->duration, 1.0f, 1e-5f);
    // The scale channel targets mesh node 3, which is not a joint: it is dropped.
    ASSERT_EQ(walk->channels.size(), 2u);

    const auto channel_for = [walk](const std::uint32_t joint) -> const VulkanEngine::AnimationChannel* {
        for (const auto& channel : walk->channels) {
            if (channel.joint == joint) return &channel;
        }
        return nullptr;
    };
    // glTF channel nodes are mapped to skeleton joints, not stored raw:
    // node 1 is joint0 and node 2 is joint1 of skeleton 0.
    const VulkanEngine::AnimationChannel* translation = channel_for(0);
    ASSERT_NE(translation, nullptr);
    EXPECT_EQ(translation->path, AnimationPath::Translation);
    EXPECT_EQ(translation->interpolation, AnimationInterpolation::Linear);
    ASSERT_EQ(translation->keys.size(), 2u);
    EXPECT_NEAR(translation->keys[0].time, 0.0f, 1e-6f);
    EXPECT_NEAR(translation->keys[1].time, 1.0f, 1e-6f);
    EXPECT_NEAR(translation->keys[0].value[0], 0.0f, 1e-6f);
    EXPECT_NEAR(translation->keys[1].value[0], 1.0f, 1e-6f);

    const VulkanEngine::AnimationChannel* rotation = channel_for(1);
    ASSERT_NE(rotation, nullptr);
    EXPECT_EQ(rotation->path, AnimationPath::Rotation);
    ASSERT_EQ(rotation->keys.size(), 2u);
    EXPECT_NEAR(rotation->keys[1].value[2], 0.70710678f, 1e-5f);
    EXPECT_NEAR(rotation->keys[1].value[3], 0.70710678f, 1e-5f);
}

TEST_F(GltfAnimationImportTest, CubicSplineKeepsTheMiddleValueAsLinearKeys) {
    const std::shared_ptr<Mesh> mesh = AssembleAnimated();
    ASSERT_NE(mesh, nullptr);

    const AnimationClip* cubic = FindClip(*mesh, "cubic");
    ASSERT_NE(cubic, nullptr);
    EXPECT_EQ(cubic->skeleton, 0u);
    ASSERT_EQ(cubic->channels.size(), 1u);

    const VulkanEngine::AnimationChannel& channel = cubic->channels[0];
    // The output accessor holds (in-tangent, value, out-tangent) per key; the
    // runtime type keeps only the value and stores it as Linear.
    EXPECT_EQ(channel.interpolation, AnimationInterpolation::Linear);
    ASSERT_EQ(channel.keys.size(), 2u);
    EXPECT_NEAR(channel.keys[0].value[0], 1.0f, 1e-6f);
    EXPECT_NEAR(channel.keys[0].value[1], 2.0f, 1e-6f);
    EXPECT_NEAR(channel.keys[0].value[2], 3.0f, 1e-6f);
    EXPECT_NEAR(channel.keys[1].value[0], 4.0f, 1e-6f);
    EXPECT_NEAR(channel.keys[1].value[1], 5.0f, 1e-6f);
    EXPECT_NEAR(channel.keys[1].value[2], 6.0f, 1e-6f);
    EXPECT_NEAR(cubic->duration, 1.0f, 1e-5f);
}

TEST_F(GltfAnimationImportTest, AnimationTargetingTwoSkeletonsIsSplit) {
    const std::shared_ptr<Mesh> mesh = AssembleAnimated();
    ASSERT_NE(mesh, nullptr);

    const AnimationClip* first = FindClip(*mesh, "multi#0");
    const AnimationClip* second = FindClip(*mesh, "multi#1");
    ASSERT_NE(first, nullptr) << "skeletons 0 and 1 must produce two clips";
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(first->skeleton, 0u);
    EXPECT_EQ(second->skeleton, 1u);
    ASSERT_EQ(first->channels.size(), 1u);
    ASSERT_EQ(second->channels.size(), 1u);
    EXPECT_EQ(first->channels[0].joint, 0u);
    EXPECT_EQ(second->channels[0].joint, 0u);
    EXPECT_NEAR(first->duration, 1.0f, 1e-5f);
    EXPECT_NEAR(second->duration, 1.0f, 1e-5f);
}

TEST_F(GltfAnimationImportTest, OverlappingSkeletonsBothReceiveTheChannel) {
    // skin 0 covers [joint0, joint1], skin 1 covers only [joint0]: two skeletons
    // with different ordered joint lists share node 1. A channel targeting node 1
    // must reach both skeletons, or the second pose would stay in bind pose.
    const auto path = WriteFixture(dir_, "overlapping.glb", OverlappingSkeletonGlb());
    const std::shared_ptr<Mesh> mesh = Assemble(path);
    ASSERT_NE(mesh, nullptr);
    ASSERT_EQ(mesh->skeletons.size(), 2u);
    ASSERT_EQ(mesh->animations.size(), 2u);

    const AnimationClip* first = FindClip(*mesh, "overlap#0");
    const AnimationClip* second = FindClip(*mesh, "overlap#1");
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(first->skeleton, 0u);
    EXPECT_EQ(second->skeleton, 1u);
    ASSERT_EQ(first->channels.size(), 1u);
    ASSERT_EQ(second->channels.size(), 1u);
    // joint0 is index 0 in both skeletons, but the mapping was resolved per
    // skeleton rather than copied from the first.
    EXPECT_EQ(first->channels[0].joint, 0u);
    EXPECT_EQ(second->channels[0].joint, 0u);
}

TEST_F(GltfAnimationImportTest, NoClipStoresRawNodeIndices) {
    const std::shared_ptr<Mesh> mesh = AssembleAnimated();
    ASSERT_NE(mesh, nullptr);

    for (const AnimationClip& clip : mesh->animations) {
        ASSERT_LT(clip.skeleton, mesh->skeletons.size());
        for (const auto& channel : clip.channels) {
            // Every channel joint must index the clip's own skeleton.
            EXPECT_LT(channel.joint, mesh->skeletons[clip.skeleton].joints.size());
            EXPECT_FALSE(channel.keys.empty());
        }
    }
}

} // namespace
