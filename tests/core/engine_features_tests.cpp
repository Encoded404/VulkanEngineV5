// Device-free EngineFeatures / preset-selection tests (examples -> selective
// engine modules migration).
//
// Constructing a GameEngine runs the preset registration but touches no
// device: RegisterEngineDefaultModules only records module descriptors, so the
// selected slot set is observable here without a Vulkan device. Importing
// VulkanEngine.GameEngine pulls the whole engine graph, which is why these tests
// do not live in the narrow EngineModules test.

#include <gtest/gtest.h>

import std;

import VulkanEngine.GameEngine;

namespace {

using namespace VulkanEngine;
using namespace VulkanEngine::Modules;

} // namespace

TEST(EngineFeaturesTest, DefaultsToTheDirectModePreset) {
    const EngineFeatures features{};

    EXPECT_TRUE(features.default_renderer);
    EXPECT_FALSE(features.text);
    EXPECT_FALSE(features.imgui);
#if defined(VKENGINE_PHYSICS)
    EXPECT_FALSE(features.physics);
#endif
#if defined(VKENGINE_PHYSICAL_CAMERA)
    EXPECT_FALSE(features.physical_camera);
#endif
}

TEST(EngineFeaturesTest, TextOnlyDoesNotRequestOtherSubsystems) {
    const EngineFeatures features{.text = true};

    EXPECT_TRUE(features.default_renderer);
    EXPECT_TRUE(features.text);
    EXPECT_FALSE(features.imgui);
#if defined(VKENGINE_PHYSICS)
    EXPECT_FALSE(features.physics);
#endif
#if defined(VKENGINE_PHYSICAL_CAMERA)
    EXPECT_FALSE(features.physical_camera);
#endif
}

TEST(EngineFeaturesTest, AllRequestsEveryAvailableSubsystem) {
    constexpr EngineFeatures features = EngineFeatures::All();

    EXPECT_TRUE(features.default_renderer);
    EXPECT_TRUE(features.text);
    EXPECT_TRUE(features.imgui);
#if defined(VKENGINE_PHYSICS)
    EXPECT_TRUE(features.physics);
#endif
#if defined(VKENGINE_PHYSICAL_CAMERA)
    EXPECT_TRUE(features.physical_camera);
#endif
}

TEST(EngineFeaturesTest, EmptyPresetLeavesOptionalSlotsUnbound) {
    GameEngine engine{EngineFeatures{}};

    EXPECT_TRUE(engine.Modules().HasSlot(ModuleSlot::Device));
    EXPECT_TRUE(engine.Modules().HasSlot(ModuleSlot::ResourceHooks));
    EXPECT_TRUE(engine.Modules().HasSlot(ModuleSlot::FramePipeline));
    // The default renderer is the direct-mode template's renderer: a game that
    // does not replace it always has one.
    EXPECT_TRUE(engine.Modules().HasSlot(ModuleSlot::Renderer));
    EXPECT_TRUE(engine.Modules().HasSlot(ModuleSlot::ShaderWatch));

    EXPECT_FALSE(engine.Modules().HasSlot(ModuleSlot::Text));
    EXPECT_FALSE(engine.Modules().HasSlot(ModuleSlot::Physics));
    EXPECT_FALSE(engine.Modules().HasSlot(ModuleSlot::PhysicalCamera));
    EXPECT_FALSE(engine.Modules().HasSlot(ModuleSlot::ImGui));
}

TEST(EngineFeaturesTest, TextPresetBindsTheTextSlot) {
    GameEngine engine{EngineFeatures{.text = true}};

    EXPECT_TRUE(engine.Modules().HasSlot(ModuleSlot::Text));
    EXPECT_TRUE(engine.Modules().HasSlot(ModuleSlot::Device));
}

TEST(EngineFeaturesTest, ImGuiPresetAddsTheOverlayModuleBeforeTheRenderer) {
    GameEngine engine{EngineFeatures{.imgui = true}};

    EXPECT_TRUE(engine.Modules().HasSlot(ModuleSlot::ImGui));
    EXPECT_TRUE(engine.Modules().HasSlot(ModuleSlot::Renderer));
    EXPECT_TRUE(engine.Modules().HasSlot(ModuleSlot::ShaderWatch));
    EXPECT_FALSE(engine.Modules().HasSlot(ModuleSlot::Text));
}

TEST(EngineFeaturesTest, DisablingTheDefaultRendererLeavesTheRenderSlotsUnbound) {
    // ImGui and ShaderWatch are the renderer preset's companions: a game that
    // supplies its own renderer supplies those too.
    GameEngine engine{EngineFeatures{.default_renderer = false, .imgui = true}};

    EXPECT_FALSE(engine.Modules().HasSlot(ModuleSlot::Renderer));
    EXPECT_FALSE(engine.Modules().HasSlot(ModuleSlot::ImGui));
    EXPECT_FALSE(engine.Modules().HasSlot(ModuleSlot::ShaderWatch));
    EXPECT_TRUE(engine.Modules().HasSlot(ModuleSlot::Device));
}

TEST(EngineFeaturesTest, PhysicsPresetBindsThePhysicsSlotOnlyWhenCompiledIn) {
#if defined(VKENGINE_PHYSICS)
    GameEngine engine{EngineFeatures{.physics = true}};
    EXPECT_TRUE(engine.Modules().HasSlot(ModuleSlot::Physics));
    EXPECT_TRUE(engine.Modules().HasSlot(ModuleSlot::Device));
    EXPECT_FALSE(engine.Modules().HasSlot(ModuleSlot::Text));
#endif
}

TEST(EngineFeaturesTest, DefaultConstructorSelectsEveryAvailableFeature) {
    GameEngine engine{};

    const EngineFeatures expected = EngineFeatures::All();
    EXPECT_EQ(engine.Features().default_renderer, expected.default_renderer);
    EXPECT_EQ(engine.Features().text, expected.text);
    EXPECT_EQ(engine.Features().imgui, expected.imgui);

    EXPECT_TRUE(engine.Modules().HasSlot(ModuleSlot::Device));
    EXPECT_TRUE(engine.Modules().HasSlot(ModuleSlot::Text));
    EXPECT_TRUE(engine.Modules().HasSlot(ModuleSlot::ResourceHooks));
    EXPECT_TRUE(engine.Modules().HasSlot(ModuleSlot::FramePipeline));
    EXPECT_TRUE(engine.Modules().HasSlot(ModuleSlot::Renderer));
    EXPECT_TRUE(engine.Modules().HasSlot(ModuleSlot::ImGui));
    EXPECT_TRUE(engine.Modules().HasSlot(ModuleSlot::ShaderWatch));
#if defined(VKENGINE_PHYSICS)
    EXPECT_TRUE(engine.Modules().HasSlot(ModuleSlot::Physics));
#endif
#if defined(VKENGINE_PHYSICAL_CAMERA)
    EXPECT_TRUE(engine.Modules().HasSlot(ModuleSlot::PhysicalCamera));
#endif
}
