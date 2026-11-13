// Device-free ECS schedule profiling tests.
//
// The schedules accept a non-owning VulkanShared::ISystemProfileSink; these
// tests pin the contract: one call per executed system, in execution order,
// with the right kind, the resolved label, and a non-negative elapsed time.
// A null sink must leave the existing ordering behaviour untouched.

#include <gtest/gtest.h>

import std;

import VulkanShared.Jobs;
import VulkanShared.SystemProfile;
import VulkanEngine.Application;
import VulkanEngine.EngineContext;
import VulkanEngine.ECS.FrameSchedule;
import VulkanEngine.ECS.SystemSchedule;

namespace {

using namespace VulkanEngine;
using VulkanShared::SystemProfileKind;

struct RecordedTiming {
    SystemProfileKind kind;
    std::string label;
    double ms = 0.0;
};

class RecordingSink final : public VulkanShared::ISystemProfileSink {
public:
    void AddSystemTiming(const SystemProfileKind kind, const std::string_view label,
                         const double milliseconds) override {
        calls.push_back({kind, std::string(label), milliseconds});
    }

    std::vector<RecordedTiming> calls;
};

} // namespace

TEST(EcsProfileTest, FrameScheduleReportsEverySystemInExecutionOrder) {
    EngineContext engine{};
    Application::ApplicationContext app{};
    FramePhaseContext fctx{&app, &engine};

    FrameSchedule frames;
    frames.Add(FramePhase::Bridge, [](FramePhaseContext&) {}, "game.first");
    frames.Add(FramePhase::Bridge, [](FramePhaseContext&) {}, "game.second");
    frames.Add(FramePhase::Record, [](FramePhaseContext&) {}, "game.third");

    RecordingSink sink;
    frames.Run(FramePhase::Bridge, fctx, &sink);

    ASSERT_EQ(sink.calls.size(), 2u);
    EXPECT_EQ(sink.calls[0].kind, SystemProfileKind::Frame);
    EXPECT_EQ(sink.calls[0].label, "game.first");
    EXPECT_EQ(sink.calls[1].kind, SystemProfileKind::Frame);
    EXPECT_EQ(sink.calls[1].label, "game.second");
    EXPECT_GE(sink.calls[0].ms, 0.0);
    EXPECT_GE(sink.calls[1].ms, 0.0);

    // Running another phase reports only that phase's systems.
    sink.calls.clear();
    frames.Run(FramePhase::Record, fctx, &sink);
    ASSERT_EQ(sink.calls.size(), 1u);
    EXPECT_EQ(sink.calls[0].label, "game.third");
}

TEST(EcsProfileTest, FrameScheduleFallsBackToSlotNameThenRegistrationIndex) {
    EngineContext engine{};
    Application::ApplicationContext app{};
    FramePhaseContext fctx{&app, &engine};

    FrameSchedule frames;
    // Slot-bound, unnamed: the slot's diagnostic spelling is the label.
    frames.Add(FramePhase::Record, FrameSlot::RenderSubmit, [](FramePhaseContext&) {});
    frames.Run(FramePhase::Record, fctx);

    RecordingSink sink;
    frames.Run(FramePhase::Record, fctx, &sink);
    ASSERT_EQ(sink.calls.size(), 1u);
    EXPECT_EQ(sink.calls[0].label, "frame.render_submit");

    FrameSchedule unnamed;
    unnamed.Add(FramePhase::Bridge, [](FramePhaseContext&) {});
    unnamed.Run(FramePhase::Bridge, fctx, &sink);
    ASSERT_EQ(sink.calls.size(), 2u);
    EXPECT_EQ(sink.calls[1].label, "frame_system[0]");
}

TEST(EcsProfileTest, NullSinkLeavesFrameOrderingUnchanged) {
    EngineContext engine{};
    Application::ApplicationContext app{};
    FramePhaseContext fctx{&app, &engine};

    std::vector<std::string> order;
    FrameSchedule frames;
    frames.Add(FramePhase::Bridge, [&order](FramePhaseContext&) { order.push_back("a"); }, "a");
    frames.Add(FramePhase::Bridge, [&order](FramePhaseContext&) { order.push_back("b"); }, "b");

    frames.Run(FramePhase::Bridge, fctx);
    EXPECT_EQ(order, (std::vector<std::string>{"a", "b"}));
}

TEST(EcsProfileTest, SystemScheduleReportsEverySystemInExecutionOrder) {
    ComponentRegistry registry{};
    SystemSchedule systems;
    systems.Add([](ComponentRegistry&, float) {}, "physics.custom_a");
    systems.Add([](ComponentRegistry&, float) {}, "physics.custom_b");

    RecordingSink sink;
    systems.Run(registry, 0.016f, &sink);

    ASSERT_EQ(sink.calls.size(), 2u);
    EXPECT_EQ(sink.calls[0].kind, SystemProfileKind::Component);
    EXPECT_EQ(sink.calls[0].label, "physics.custom_a");
    EXPECT_EQ(sink.calls[1].kind, SystemProfileKind::Component);
    EXPECT_EQ(sink.calls[1].label, "physics.custom_b");
    EXPECT_GE(sink.calls[0].ms, 0.0);
}

TEST(EcsProfileTest, SystemScheduleFallsBackToSlotNameThenRegistrationIndex) {
    ComponentRegistry registry{};
    SystemSchedule systems;
    systems.Add(SystemSlot::PhysicsSimulate, [](ComponentRegistry&, float) {});

    RecordingSink sink;
    systems.Run(registry, 0.016f, &sink);
    ASSERT_EQ(sink.calls.size(), 1u);
    EXPECT_EQ(sink.calls[0].label, "physics.simulate");

    SystemSchedule unnamed;
    unnamed.Add([](ComponentRegistry&, float) {});
    unnamed.Run(registry, 0.016f, &sink);
    ASSERT_EQ(sink.calls.size(), 2u);
    EXPECT_EQ(sink.calls[1].label, "system[0]");
}

TEST(EcsProfileTest, SystemScheduleNullSinkStillRunsSystems) {
    ComponentRegistry registry{};
    int runs = 0;
    SystemSchedule systems;
    systems.Add([&runs](ComponentRegistry&, float) { ++runs; }, "count");

    systems.Run(registry, 0.016f);
    EXPECT_EQ(runs, 1);
}

// ── Object-component update: one timing per executed plan segment ──

namespace {

using namespace VulkanEngine;

struct ProfileTarget : Component {
    static constexpr std::string_view kName = "test.profile_target";
};

class ProfileExclusiveA : public Component {
public:
    static constexpr std::string_view kName = "test.profile_exclusive_a";
    void Update(float, UpdateScope&) override {}
};
class ProfileExclusiveB : public Component {
public:
    static constexpr std::string_view kName = "test.profile_exclusive_b";
    void Update(float, UpdateScope&) override {}
};
class ProfileExclusiveC : public Component {
public:
    static constexpr std::string_view kName = "test.profile_exclusive_c";
    void Update(float, UpdateScope&) override {}
};

class ProfileWriter : public Component {
public:
    using SelfWrites = AccessSet<ProfileTarget>;
    static constexpr Concurrency kConcurrency = Concurrency::Serial;
    static constexpr std::string_view kName = "test.profile_writer";
    void Update(float, UpdateScope&) override {}
};
class ProfileReaderOne : public Component {
public:
    using SelfReads = AccessSet<ProfileTarget>;
    static constexpr Concurrency kConcurrency = Concurrency::SelfParallel;
    static constexpr std::string_view kName = "test.profile_reader_one";
    void Update(float, UpdateScope&) override {}
};
class ProfileReaderTwo : public Component {
public:
    using SelfReads = AccessSet<ProfileTarget>;
    static constexpr Concurrency kConcurrency = Concurrency::SelfParallel;
    static constexpr std::string_view kName = "test.profile_reader_two";
    void Update(float, UpdateScope&) override {}
};

} // namespace

TEST(EcsProfileTest, ComponentUpdateReportsOneTimingPerExecutedSegment) {
    ComponentRegistry registry;
    Entity& entity = registry.CreateEntity();
    registry.AddComponent<ProfileExclusiveA>(entity);
    registry.AddComponent<ProfileExclusiveB>(entity);
    registry.AddComponent<ProfileExclusiveC>(entity);

    // Three exclusive types: three levels, each with one unit, merged into one
    // sequential segment.
    const std::vector<PlanSegment> plan = registry.ExecutionPlan();
    ASSERT_EQ(plan.size(), 1u);
    ASSERT_EQ(plan[0].level_count, 3u);

    RecordingSink sink;
    registry.UpdateComponents(0.016f, {}, &sink);

    ASSERT_EQ(sink.calls.size(), plan.size());
    EXPECT_EQ(sink.calls[0].kind, SystemProfileKind::Component);
    EXPECT_EQ(sink.calls[0].label, "component.serial[0-2]");
    EXPECT_GE(sink.calls[0].ms, 0.0);
}

TEST(EcsProfileTest, AParallelSegmentIsLabelledByLevelIndex) {
    auto scheduler = VulkanShared::JobScheduler{VulkanShared::JobBudget{.workers = 2}};
    ComponentRegistry registry{&scheduler};
    Entity& entity = registry.CreateEntity();
    registry.AddComponent<ProfileWriter>(entity);
    registry.AddComponent<ProfileReaderOne>(entity);
    registry.AddComponent<ProfileReaderTwo>(entity);

    // [writer] [reader_one, reader_two]: the first level holds one unit and the
    // second holds two, so only the second is dispatched in parallel.
    ASSERT_EQ(registry.Schedule().Levels().size(), 2u);

    RecordingSink sink;
    registry.UpdateComponents(0.016f, {}, &sink);

    ASSERT_EQ(sink.calls.size(), 2u);
    EXPECT_EQ(sink.calls[0].label, "component.serial[0]");
    EXPECT_EQ(sink.calls[1].label, "component.parallel[1]");
    for (const RecordedTiming& call : sink.calls) {
        EXPECT_EQ(call.kind, SystemProfileKind::Component);
        EXPECT_GE(call.ms, 0.0);
    }
}

TEST(EcsProfileTest, ANullSinkRecordsNothing) {
    ComponentRegistry registry;
    Entity& entity = registry.CreateEntity();
    registry.AddComponent<ProfileExclusiveA>(entity);
    registry.AddComponent<ProfileExclusiveB>(entity);

    // Nothing to assert beyond "it ran and did not need a sink".
    EXPECT_NO_THROW(registry.UpdateComponents(0.016f, {}, nullptr));
}
