// Device-free tests for GameEngine's modifier-registration surface.
//
// GameEngine::RegisterModifier/UnregisterModifier forward to the context-owned
// ModifierManager, but they are the only way an application reaches the
// registry, so the seam is what these tests pin: registering before Setup is
// legal (the renderer builds pipelines on its first frame, not at
// registration), the returned handle resolves through GetModifierManager, and
// an invalid descriptor is rejected exactly as the manager rejects it.
//
// Constructing a GameEngine runs the preset registration but touches no device,
// the same contract engine_features_tests relies on.

#include <gtest/gtest.h>

import std;

import VulkanEngine.GameEngine;

namespace {

using namespace VulkanEngine;
using namespace VulkanEngine::Modifiers;

// Registration only validates that a shader id is present; building the pipeline
// is ModifierSync's job on a device. Any non-sentinel id therefore exercises the
// device-free path.
constexpr ShaderSystem::ShaderId kFakeShader{1};

[[nodiscard]] ModifierDesc MakeDesc(std::string name, const std::uint32_t param_size = 16) {
    return ModifierDesc{
        .name = std::move(name),
        .compute_shader = kFakeShader,
        .entry_point = "main",
        .param_size = param_size,
        .param_alignment = 16,
        .workgroup_size = 64,
    };
}

} // namespace

TEST(GameEngineModifierTest, RegisterModifierBeforeSetupIsLegalAndVisible) {
    GameEngine engine{EngineFeatures{}};
    const std::uint64_t before = engine.GetModifierManager().RegistryVersion();

    const ModifierId id = engine.RegisterModifier(MakeDesc("app_modifier"));

    ASSERT_TRUE(id.IsValid());
    // The version bump is what makes a renderer notice the new entry: without it
    // SyncModifierPipelines never builds the pipeline and gather never emits it.
    EXPECT_GT(engine.GetModifierManager().RegistryVersion(), before);
    const ModifierDesc* desc = engine.GetModifierManager().Get(id);
    ASSERT_NE(desc, nullptr);
    EXPECT_EQ(desc->name, "app_modifier");
    EXPECT_EQ(desc->param_size, 16u);
    EXPECT_FALSE(desc->builtin) << "an app modifier must not be unremovable";
}

TEST(GameEngineModifierTest, UnregisterModifierRetiresTheHandle) {
    GameEngine engine{EngineFeatures{}};
    const ModifierId id = engine.RegisterModifier(MakeDesc("app_modifier"));
    ASSERT_TRUE(id.IsValid());
    const std::uint64_t after_register = engine.GetModifierManager().RegistryVersion();

    engine.UnregisterModifier(id);

    EXPECT_FALSE(engine.GetModifierManager().IsAlive(id));
    EXPECT_EQ(engine.GetModifierManager().Get(id), nullptr);
    EXPECT_GT(engine.GetModifierManager().RegistryVersion(), after_register);
    // A second unregister, or an invalid handle, is a harmless no-op.
    engine.UnregisterModifier(id);
    engine.UnregisterModifier(kInvalidModifierId);
}

TEST(GameEngineModifierTest, AnInvalidDescriptorIsRejected) {
    GameEngine engine{EngineFeatures{}};
    const std::uint64_t before = engine.GetModifierManager().RegistryVersion();

    ModifierDesc no_shader = MakeDesc("no_shader");
    no_shader.compute_shader = ShaderSystem::kInvalidShaderId;
    EXPECT_FALSE(engine.RegisterModifier(std::move(no_shader)).IsValid());

    // Out-of-range parameter size: kMaxParamBytes is 4096.
    EXPECT_FALSE(engine.RegisterModifier(MakeDesc("too_big", 4097)).IsValid());

    EXPECT_EQ(engine.GetModifierManager().RegistryVersion(), before)
        << "a rejected descriptor must not look like a change to a renderer";
}
