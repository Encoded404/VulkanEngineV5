#include <gtest/gtest.h>

import std;

import VulkanEngine.ECS.SystemSchedule;

namespace {

using VulkanEngine::ComponentRegistry;
using VulkanEngine::SystemSchedule;

struct Counter {
    int value = 0;
};

} // namespace

TEST(SystemScheduleTest, RunsSystemsInRegistrationOrder) {
    ComponentRegistry registry;
    auto& entity = registry.CreateEntity();
    registry.AddComponent<Counter>(entity);

    std::vector<std::string> order;
    SystemSchedule schedule;
    schedule.Add("first", [&](ComponentRegistry& reg, float) {
        order.push_back("first");
        if (auto* counter = reg.GetComponent<Counter>(entity)) counter->value += 1;
    });
    schedule.Add("second", [&](ComponentRegistry&, float) { order.push_back("second"); });
    schedule.Add("third", [&](ComponentRegistry&, float) { order.push_back("third"); });

    ASSERT_EQ(schedule.Size(), 3u);
    schedule.Run(registry, 0.016f);

    const std::vector<std::string> expected{"first", "second", "third"};
    EXPECT_EQ(order, expected);
    EXPECT_EQ(registry.GetComponent<Counter>(entity)->value, 1);
}

TEST(SystemScheduleTest, EmptyScheduleRunsNothing) {
    ComponentRegistry registry;
    SystemSchedule schedule;
    EXPECT_EQ(schedule.Size(), 0u);
    schedule.Run(registry, 0.016f); // must not crash
    SUCCEED();
}

TEST(SystemScheduleTest, ClearRemovesEverySystem) {
    SystemSchedule schedule;
    schedule.Add("a", [](ComponentRegistry&, float) {});
    ASSERT_EQ(schedule.Size(), 1u);

    schedule.Clear();
    EXPECT_EQ(schedule.Size(), 0u);
}

TEST(SystemScheduleTest, DestroyOutsideIterationAppliesImmediately) {
    ComponentRegistry registry;
    auto& doomed = registry.CreateEntity();
    registry.AddComponent<Counter>(doomed);
    const auto doomed_id = doomed.GetId();

    SystemSchedule schedule;
    schedule.Add("destroy-doomed", [&](ComponentRegistry& reg, float) {
        if (auto* entity = reg.TryGetEntity(doomed_id)) reg.DestroyEntity(*entity);
    });
    schedule.Run(registry, 0.0f);

    // Nothing was iterating, so the queue fast-path applied it at once.
    EXPECT_EQ(registry.Count<Counter>(), 0u);
    EXPECT_FALSE(registry.IsAlive(doomed_id));
}

TEST(SystemScheduleTest, DestroyInsideAViewIsDeferredUntilThePhaseBoundary) {
    ComponentRegistry registry;
    auto& keep = registry.CreateEntity();
    registry.AddComponent<Counter>(keep);
    auto& doomed = registry.CreateEntity();
    registry.AddComponent<Counter>(doomed);
    const auto doomed_id = doomed.GetId();

    SystemSchedule schedule;
    schedule.Add("destroy-doomed", [&](ComponentRegistry& reg, float) {
        reg.ForEach<Counter>([&](VulkanEngine::Entity& entity, Counter&) {
            if (entity.GetId() == doomed_id) reg.DestroyEntity(entity);
        });
    });
    schedule.Run(registry, 0.0f);

    // Still present until the fixed-update boundary applies the queue.
    EXPECT_EQ(registry.Count<Counter>(), 2u);
    registry.ApplyStructuralChanges();
    EXPECT_EQ(registry.Count<Counter>(), 1u);
    EXPECT_FALSE(registry.IsAlive(doomed_id));
}
