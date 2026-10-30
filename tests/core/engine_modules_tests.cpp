// Device-free engine-module registry tests (rung 2 of engine modularization).
//
// The lifecycle seam's contract must be ordered, deterministic, and unwound in
// exact reverse start order. Nothing here creates a Vulkan device: the registry
// only orders modules and calls their callbacks.

#include <gtest/gtest.h>

import std;

import VulkanEngine.Application;
import VulkanEngine.EngineContext;
import VulkanEngine.ECS.ComponentRegistry;
import VulkanEngine.ECS.SystemSchedule;
import VulkanEngine.ECS.FrameSchedule;
import VulkanEngine.Modules.EngineModules;

namespace {

using namespace VulkanEngine;
using namespace VulkanEngine::Modules;

/// A fresh ModuleContext over device-free stand-ins (null subsystem pointers
/// are legal for modules that touch none of them).
ModuleContext FreshContext() {
    static Application::ApplicationContext app{};
    static EngineContext engine{};
    static ComponentRegistry registry{};
    static SystemSchedule systems{};
    static FrameSchedule frames{};
    static GameConfig config{};
    ModuleContext mctx{};
    mctx.app = &app;
    mctx.engine = &engine;
    mctx.registry = &registry;
    mctx.fixed_systems = &systems;
    mctx.frame_systems = &frames;
    mctx.config = &config;
    return mctx;
}

} // namespace

TEST(EngineModulesTest, StartupRunsInRegistrationOrder) {
    std::vector<std::string> started;
    EngineModules modules;
    modules.Add(EngineModuleDesc{
        .stage = Stage::Startup,
        .label = "first",
        .startup = [&](ModuleContext&) { started.push_back("first"); return true; },
    });
    modules.Add(EngineModuleDesc{
        .stage = Stage::Startup,
        .label = "second",
        .startup = [&](ModuleContext&) { started.push_back("second"); return true; },
    });

    ModuleContext mctx = FreshContext();
    ASSERT_TRUE(modules.StartupAll(Stage::Startup, mctx));
    EXPECT_EQ(started, (std::vector<std::string>{"first", "second"}));
}

TEST(EngineModulesTest, ConfigureRunsBeforeStartupInResolvedOrder) {
    EngineModules modules;
    std::vector<std::string> calls;
    modules.Add(EngineModuleDesc{
        .stage = Stage::Startup,
        .label = "a",
        .configure = [&](GameConfig& config) {
            calls.push_back("config.a");
            // declare a default the game may later override (it will not)
            config.job_workers = 2;
        },
        .startup = [&](ModuleContext&) { calls.push_back("start.a"); return true; },
    });
    modules.Add(EngineModuleDesc{
        .stage = Stage::Startup,
        .label = "b",
        .configure = [&](GameConfig&) { calls.push_back("config.b"); },
        .startup = [&](ModuleContext&) { calls.push_back("start.b"); return true; },
    });

    GameConfig config{};
    ModuleContext mctx = FreshContext();
    modules.ConfigureAll(config);
    ASSERT_TRUE(modules.StartupAll(Stage::Startup, mctx));

    // Every Configure ran before any startup: the game's overrides are final
    // before a module starts.
    const std::vector<std::string> expected{
        "config.a", "config.b", "start.a", "start.b",
    };
    EXPECT_EQ(calls, expected);
}

TEST(EngineModulesTest, ShutdownIsTheExactReverseOfStartupAcrossStages) {
    std::vector<std::string> calls;
    EngineModules modules;
    modules.Add(EngineModuleDesc{
        .stage = Stage::Startup, .label = "device", .slot = ModuleSlot::Device,
        .startup = [&](ModuleContext&) { calls.push_back("start.device"); return true; },
        .shutdown = [&](ModuleContext&) { calls.push_back("stop.device"); },
    });
    modules.AddAfter(ModuleSlot::Device, EngineModuleDesc{
        .stage = Stage::Startup, .label = "physics",
        .startup = [&](ModuleContext&) { calls.push_back("start.physics"); return true; },
        .shutdown = [&](ModuleContext&) { calls.push_back("stop.physics"); },
    });
    modules.Add(EngineModuleDesc{
        .stage = Stage::Scene, .label = "scene",
        .startup = [&](ModuleContext&) { calls.push_back("start.scene"); return true; },
        .shutdown = [&](ModuleContext&) { calls.push_back("stop.scene"); },
    });

    ModuleContext mctx = FreshContext();
    ASSERT_TRUE(modules.StartupAll(Stage::Startup, mctx));
    ASSERT_TRUE(modules.StartupAll(Stage::Scene, mctx));
    calls.clear();
    modules.ShutdownAll(mctx);

    // Exact reverse of the full start history: scene, physics, device.
    EXPECT_EQ(calls, (std::vector<std::string>{"stop.scene", "stop.physics", "stop.device"}));

    // Idempotent: a second ShutdownAll must not double-stop anyone.
    modules.ShutdownAll(mctx);
    EXPECT_EQ(calls, (std::vector<std::string>{"stop.scene", "stop.physics", "stop.device"}));
}

TEST(EngineModulesTest, StartedOnlyModulesAreUnwoundOnStartupFailure) {
    std::vector<std::string> calls;
    EngineModules modules;
    modules.Add(EngineModuleDesc{
        .stage = Stage::Startup, .label = "device",
        .startup = [&](ModuleContext&) { calls.push_back("start.device"); return true; },
        .shutdown = [&](ModuleContext&) { calls.push_back("stop.device"); },
    });
    modules.Add(EngineModuleDesc{
        .stage = Stage::Startup, .label = "dies",
        .startup = [&](ModuleContext&) { calls.push_back("start.dies"); return false; },
        .shutdown = [&](ModuleContext&) { calls.push_back("stop.dies"); },
    });
    modules.Add(EngineModuleDesc{
        .stage = Stage::Startup, .label = "never",
        .startup = [&](ModuleContext&) { calls.push_back("start.never"); return true; },
    });

    ModuleContext mctx = FreshContext();
    EXPECT_FALSE(modules.StartupAll(Stage::Startup, mctx));

    // The engine unwinds a failed Setup through ShutdownAll.
    modules.ShutdownAll(mctx);
    EXPECT_EQ(calls, (std::vector<std::string>{"start.device", "start.dies", "stop.dies", "stop.device"}));
}

TEST(EngineModulesTest, SlotConstraintOrdersAcrossRegistrationOrder) {
    std::vector<std::string> started;
    EngineModules modules;
    modules.Add(EngineModuleDesc{
        .stage = Stage::Startup, .label = "device", .slot = ModuleSlot::Device,
        .startup = [&](ModuleContext&) { started.push_back("device"); return true; },
    });
    // Registered later, but constrained after the device slot.
    modules.AddAfter(ModuleSlot::Device, EngineModuleDesc{
        .stage = Stage::Startup, .label = "game",
        .startup = [&](ModuleContext&) { started.push_back("game"); return true; },
    });

    ModuleContext mctx = FreshContext();
    ASSERT_TRUE(modules.StartupAll(Stage::Startup, mctx));
    EXPECT_EQ(started, (std::vector<std::string>{"device", "game"}));
}

TEST(EngineModulesTest, AFailedModuleDoesNotStopLaterModulesOfAnotherStage) {
    std::vector<std::string> started;
    EngineModules modules;
    modules.Add(EngineModuleDesc{
        .stage = Stage::Scene, .label = "scene",
        .startup = [&](ModuleContext&) { started.push_back("scene"); return true; },
    });

    ModuleContext mctx = FreshContext();
    ASSERT_TRUE(modules.StartupAll(Stage::Startup, mctx));
    EXPECT_TRUE(started.empty()) << "a Scene module never starts in the startup stage";
    ASSERT_TRUE(modules.StartupAll(Stage::Scene, mctx));
    EXPECT_EQ(started, (std::vector<std::string>{"scene"}));
}

TEST(EngineModulesTest, ConstraintCrossingAStageBoundaryIsRejected) {
    EngineModules modules;
    std::vector<std::string> started;
    modules.Add(EngineModuleDesc{
        .stage = Stage::Startup, .label = "device", .slot = ModuleSlot::Device,
        .startup = [&](ModuleContext&) { started.push_back("device"); return true; },
    });

    // A Scene-stage module cannot hang off the Startup-stage device slot: the
    // stage sequence is the frame's structure, not an ordering decision.
    EXPECT_THROW(modules.AddAfter(ModuleSlot::Device, EngineModuleDesc{
                        .stage = Stage::Scene, .label = "crossed",
                    }), std::logic_error);
    EXPECT_EQ(modules.Size(), 1u);
}

TEST(EngineModulesTest, DuplicateSlotIsRejectedAndLeavesTheRegistryIntact) {
    EngineModules modules;
    std::vector<std::string> started;
    modules.Add(EngineModuleDesc{
        .stage = Stage::Startup, .label = "device", .slot = ModuleSlot::Device,
        .startup = [&](ModuleContext&) { started.push_back("device"); return true; },
    });
    EXPECT_THROW(modules.Add(EngineModuleDesc{
        .stage = Stage::Startup, .label = "again", .slot = ModuleSlot::Device,
    }), std::logic_error);
    EXPECT_EQ(modules.Size(), 1u);
    ModuleContext mctx = FreshContext();
    ASSERT_TRUE(modules.StartupAll(Stage::Startup, mctx));
    EXPECT_EQ(started, (std::vector<std::string>{"device"}));
}

TEST(EngineModulesTest, ConstraintToAnUnregisteredSlotIsRejected) {
    EngineModules modules;
    EXPECT_THROW(modules.AddAfter(ModuleSlot::FramePipeline, EngineModuleDesc{
        .stage = Stage::Startup, .label = "dangling",
    }), std::out_of_range);
    EXPECT_EQ(modules.Size(), 0u);
}

TEST(EngineModulesTest, ModulesRegisterSystemsOntoBothScheduleSurfaces) {
    EngineModules modules;

    Application::ApplicationContext app{};
    EngineContext engine{};
    ComponentRegistry registry{};
    SystemSchedule systems{};
    FrameSchedule frames{};
    GameConfig config{};
    ModuleContext mctx{};
    mctx.app = &app;
    mctx.engine = &engine;
    mctx.registry = &registry;
    mctx.fixed_systems = &systems;
    mctx.frame_systems = &frames;
    mctx.config = &config;

    modules.Add(EngineModuleDesc{
        .stage = Stage::Startup, .label = "subsystem",
        .startup = [&](ModuleContext& ctx) {
            // A module registers its fixed-step system and frame-phase system
            // through the context it is handed — never an engine edit.
            ctx.FixedSystems().Add([](ComponentRegistry&, float) {}, "mod.fixed");
            ctx.FrameSystems().Add(FramePhase::Bridge, [](FramePhaseContext&) {}, "mod.bridge");
            return true;
        },
    });

    ASSERT_TRUE(modules.StartupAll(Stage::Startup, mctx));
    EXPECT_EQ(systems.Size(), 1u);
    EXPECT_EQ(frames.PhaseSize(FramePhase::Bridge), 1u);
}

TEST(EngineModulesTest, ModuleSlotNamesAreTheDiagnosticSpelling) {
    EXPECT_EQ(ModuleSlotName(ModuleSlot::Device), "engine.device");
    EXPECT_EQ(ModuleSlotName(ModuleSlot::Text), "engine.text");
    EXPECT_EQ(ModuleSlotName(ModuleSlot::Physics), "engine.physics");
    EXPECT_EQ(ModuleSlotName(ModuleSlot::None), "<none>");
    EXPECT_EQ(StageName(Stage::Startup), "startup");
    EXPECT_EQ(StageName(Stage::Scene), "scene");
}

TEST(EngineModulesTest, HasSlotTracksTheBoundIdentityOnly) {
    EngineModules modules;
    EXPECT_FALSE(modules.HasSlot(ModuleSlot::Device));
    // None is the "unbound" identity and never reports as bound.
    EXPECT_FALSE(modules.HasSlot(ModuleSlot::None));

    modules.Add(EngineModuleDesc{
        .stage = Stage::Startup, .label = "device", .slot = ModuleSlot::Device,
    });
    EXPECT_TRUE(modules.HasSlot(ModuleSlot::Device));
    EXPECT_FALSE(modules.HasSlot(ModuleSlot::Text));

    modules.Clear();
    EXPECT_FALSE(modules.HasSlot(ModuleSlot::Device));
}