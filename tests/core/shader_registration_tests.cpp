// Device-free RendererShaders / EngineShaderIds tests.
//
// The engine shader registry's unset marker is kInvalidShaderId (-1), not 0:
// Register hands out sequential ids starting at 0, so a default-constructed
// EngineShaderIds must mean "nothing registered yet" without colliding with the
// first shader. The resolution of `{}` into the engine pair lives in
// GameEngine::InitRenderer (device-bound) and is exercised by the example smoke
// runs; only the value-level validity rule is covered here.

#include <gtest/gtest.h>

import std;

import VulkanEngine.ShaderManager;
import VulkanEngine.ShaderRegistration;

namespace {

using namespace VulkanEngine;
using namespace VulkanEngine::ShaderSystem;

} // namespace

TEST(EngineShaderIdsTest, DefaultsToTheInvalidSentinelNotShaderZero) {
    const EngineShaderIds ids{};

    EXPECT_EQ(ids.main_indir_vert, kInvalidShaderId);
    EXPECT_EQ(ids.standard_mesh_frag, kInvalidShaderId);
    EXPECT_EQ(ids.ui_text_vert, kInvalidShaderId);
    EXPECT_EQ(ids.world_text_frag, kInvalidShaderId);
    EXPECT_EQ(ids.unlit_frag, kInvalidShaderId);
    EXPECT_EQ(ids.collect_write_comp, kInvalidShaderId);

    // Shader id 0 is a valid first registration; it must not double as "unset".
    EXPECT_EQ(kInvalidShaderId, static_cast<ShaderId>(-1));
    EXPECT_NE(kInvalidShaderId, ShaderId{0});
}

TEST(RendererShadersTest, EmptyMeansTheEnginePairAndIsNotAValidPair) {
    constexpr RendererShaders shaders{};

    EXPECT_EQ(shaders.vert, kInvalidShaderId);
    EXPECT_EQ(shaders.frag, kInvalidShaderId);
    EXPECT_EQ(shaders.manager, nullptr);
    EXPECT_FALSE(shaders.IsValid());
}

TEST(RendererShadersTest, APartialPairIsInvalid) {
    // Every field is required: a pair without a manager, or missing one stage,
    // is rejected instead of being partially applied.
    EXPECT_FALSE((RendererShaders{
        .vert = ShaderId{0}, .frag = ShaderId{1}, .manager = nullptr}).IsValid());
    EXPECT_FALSE((RendererShaders{
        .vert = kInvalidShaderId, .frag = ShaderId{1}, .manager = nullptr}).IsValid());
    EXPECT_FALSE((RendererShaders{
        .vert = ShaderId{0}, .frag = kInvalidShaderId, .manager = nullptr}).IsValid());
}
