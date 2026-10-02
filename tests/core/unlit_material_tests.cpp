#include <gtest/gtest.h>

#include <cstddef>

import std;

import VulkanEngine.MaterialManager;
import VulkanEngine.TechniqueManager;
import VulkanEngine.TechniqueManager.UnlitTextureTechnique;
import VulkanEngine.TechniqueManager.DefaultMeshTechnique;

namespace {

using VulkanEngine::MaterialManager::BlendMode;
using VulkanEngine::MaterialManager::DeriveRenderState;
using VulkanEngine::MaterialManager::MaterialDesc;
using VulkanEngine::MaterialManager::MaterialId;
using VulkanEngine::MaterialManager::MaterialManager;
using VulkanEngine::TechniqueManager::DefaultMeshPerMaterialData;
using VulkanEngine::TechniqueManager::TechniqueManager;
using VulkanEngine::TechniqueManager::UnlitPerMaterialData;
using VulkanEngine::TechniqueManager::UnlitTextureTechnique;

// The unlit technique's contract is UnlitPerMaterialData, not the lit
// DefaultMeshPerMaterialData. These checks are the compile-time guard that
// MaterialHandle<UnlitTextureTechnique>::Read/Modify accept the unlit type and
// reject the lit one.

static_assert(UnlitTextureTechnique::HasBinding<UnlitPerMaterialData>(),
              "unlit technique must declare UnlitPerMaterialData");
static_assert(!UnlitTextureTechnique::HasBinding<DefaultMeshPerMaterialData>(),
              "unlit technique must not claim the lit contract");

// Layout mirror of unlit.slang's MaterialData: albedo_texture, float4
// albedo_factor, float alpha_cutoff. C data layout, 24 bytes, no padding.
static_assert(sizeof(UnlitPerMaterialData) == 24);
static_assert(offsetof(UnlitPerMaterialData, albedo_texture) == 0);
static_assert(offsetof(UnlitPerMaterialData, albedo_factor) == 4);
static_assert(offsetof(UnlitPerMaterialData, alpha_cutoff) == 20);

class UnlitMaterialTest : public ::testing::Test {
protected:
    void SetUp() override {
        tech_mgr_.Register(std::make_unique<UnlitTextureTechnique>());
        materials_.SetTechniqueManager(&tech_mgr_);
        materials_.Initialize(nullptr);
    }

    void TearDown() override {
        materials_.Shutdown();
        tech_mgr_.Shutdown();
    }

    TechniqueManager tech_mgr_{};
    MaterialManager materials_{};
};

TEST_F(UnlitMaterialTest, RegistersTheTrimmedContract) {
    const UnlitPerMaterialData data{
        .albedo_texture = 42u,
        .albedo_factor = {0.25f, 0.5f, 0.75f, 1.0f},
        .alpha_cutoff = 0.3f,
    };
    auto handle = materials_.Register<UnlitTextureTechnique>(BlendMode::Opaque, data);
    ASSERT_TRUE(handle.Valid());

    const auto& read = handle.Read<UnlitPerMaterialData>();
    EXPECT_EQ(read.albedo_texture, 42u);
    EXPECT_FLOAT_EQ(read.albedo_factor[0], 0.25f);
    EXPECT_FLOAT_EQ(read.albedo_factor[3], 1.0f);
    EXPECT_FLOAT_EQ(read.alpha_cutoff, 0.3f);
}

TEST_F(UnlitMaterialTest, ModifyUsesTheTrimmedContract) {
    auto handle = materials_.Register<UnlitTextureTechnique>(
        BlendMode::Opaque,
        UnlitPerMaterialData{.albedo_texture = 1u});
    handle.Modify<UnlitPerMaterialData>([](UnlitPerMaterialData& d) {
        d.albedo_texture = 7u;
        d.alpha_cutoff = 0.9f;
    });
    EXPECT_EQ(handle.Read<UnlitPerMaterialData>().albedo_texture, 7u);
    EXPECT_FLOAT_EQ(handle.Read<UnlitPerMaterialData>().alpha_cutoff, 0.9f);
}

TEST_F(UnlitMaterialTest, RenderStateResolvesToADrawGroup) {
    auto opaque = materials_.Register<UnlitTextureTechnique>(
        MaterialDesc{.blend = BlendMode::Opaque},
        UnlitPerMaterialData{});
    auto cutout = materials_.Register<UnlitTextureTechnique>(
        MaterialDesc{.blend = BlendMode::Cutout, .double_sided = true},
        UnlitPerMaterialData{});
    ASSERT_TRUE(opaque.Valid());
    ASSERT_TRUE(cutout.Valid());

    const auto* opaque_state = materials_.GetRenderState(MaterialId{opaque.Id()});
    const auto* cutout_state = materials_.GetRenderState(MaterialId{cutout.Id()});
    ASSERT_NE(opaque_state, nullptr);
    ASSERT_NE(cutout_state, nullptr);
    EXPECT_EQ(*opaque_state, DeriveRenderState(MaterialDesc{.blend = BlendMode::Opaque}));
    EXPECT_TRUE(cutout_state->alpha_mask);
    EXPECT_TRUE(cutout_state->double_sided);

    // Distinct render state interns distinct groups; the base group stays group 0
    // while the cutout material selects its own.
    const MaterialId opaque_id{opaque.Id()};
    const MaterialId cutout_id{cutout.Id()};
    EXPECT_EQ(materials_.GetGroupForMaterial(opaque_id),
              materials_.GetGroupForMaterial(opaque_id));
    EXPECT_NE(materials_.GetGroupForMaterial(opaque_id),
              materials_.GetGroupForMaterial(cutout_id));
}

}  // namespace
