#include <gtest/gtest.h>

import std;

import VulkanEngine.TextureUploadScheduler;

namespace {

using VulkanEngine::Textures::PendingUpload;
using VulkanEngine::Textures::UploadLimits;
using VulkanEngine::Textures::UploadPlan;
using VulkanEngine::Textures::UploadScheduler;

PendingUpload Make(std::uint64_t id, std::uint64_t bytes, std::uint32_t priority,
                   std::uint64_t sequence, bool cancelled = false) {
    return PendingUpload{
        .id = id,
        .bytes = bytes,
        .priority = priority,
        .sequence = sequence,
        .cancelled = cancelled,
    };
}

TEST(UploadSchedulerTest, EmptyInputPlansNothing) {
    UploadScheduler scheduler;
    const auto plan = scheduler.Plan({}, 1024, 0);
    EXPECT_TRUE(plan.order.empty());
    EXPECT_EQ(plan.bytes_planned, 0u);
}

TEST(UploadSchedulerTest, HigherPriorityFirstThenFifo) {
    UploadScheduler scheduler;
    const std::array<PendingUpload, 4> pending{
        Make(1, 10, 0, 1),
        Make(2, 10, 5, 4),
        Make(3, 10, 5, 2),
        Make(4, 10, 1, 3),
    };
    const auto plan = scheduler.Plan(pending, 1u << 20, 0);
    ASSERT_EQ(plan.order.size(), 4u);
    EXPECT_EQ(plan.order[0].id, 3u); // priority 5, earlier sequence
    EXPECT_EQ(plan.order[1].id, 2u); // priority 5, later sequence
    EXPECT_EQ(plan.order[2].id, 4u); // priority 1
    EXPECT_EQ(plan.order[3].id, 1u); // priority 0
}

TEST(UploadSchedulerTest, SoftBudgetStopsBeforeOverrun) {
    UploadScheduler scheduler;
    const std::array<PendingUpload, 3> pending{
        Make(1, 100, 0, 1),
        Make(2, 100, 0, 2),
        Make(3, 100, 0, 3),
    };
    const auto plan = scheduler.Plan(pending, 250, 0);
    ASSERT_EQ(plan.order.size(), 2u);
    EXPECT_EQ(plan.bytes_planned, 200u);
    EXPECT_FALSE(plan.order[0].exceeds_budget);
}

TEST(UploadSchedulerTest, FirstItemProgressesEvenWhenLarge) {
    UploadScheduler scheduler;
    const std::array<PendingUpload, 2> pending{
        Make(1, 900, 0, 1), // larger than budget but must not starve
        Make(2, 10, 0, 2),
    };
    const auto plan = scheduler.Plan(pending, 1000, 0);
    ASSERT_FALSE(plan.order.empty());
    EXPECT_EQ(plan.order[0].id, 1u);
    EXPECT_FALSE(plan.order[0].exceeds_budget);
}

TEST(UploadSchedulerTest, OversizedItemIsRecordedAloneAndStopsTheFrame) {
    UploadScheduler scheduler;
    const std::array<PendingUpload, 3> pending{
        Make(1, 4096, 0, 1), // exceeds 1024 budget
        Make(2, 10, 0, 2),
        Make(3, 10, 0, 3),
    };
    const auto plan = scheduler.Plan(pending, 1024, 0);
    ASSERT_EQ(plan.order.size(), 1u);
    EXPECT_EQ(plan.order[0].id, 1u);
    EXPECT_TRUE(plan.order[0].exceeds_budget);
}

TEST(UploadSchedulerTest, CancelledItemsAreDroppedAndNotCounted) {
    UploadScheduler scheduler;
    const std::array<PendingUpload, 3> pending{
        Make(1, 10, 0, 1, /*cancelled=*/true),
        Make(2, 10, 0, 2),
        Make(3, 10, 0, 3, /*cancelled=*/true),
    };
    const auto plan = scheduler.Plan(pending, 1024, 0);
    ASSERT_EQ(plan.order.size(), 1u);
    EXPECT_EQ(plan.order[0].id, 2u);
    EXPECT_EQ(plan.cancelled_dropped, 2u);
}

TEST(UploadSchedulerTest, ItemLimitCapsTheFrame) {
    UploadScheduler scheduler;
    const std::array<PendingUpload, 5> pending{
        Make(1, 10, 0, 1), Make(2, 10, 0, 2), Make(3, 10, 0, 3),
        Make(4, 10, 0, 4), Make(5, 10, 0, 5),
    };
    UploadLimits limits{};
    limits.max_items = 3;
    const auto plan = scheduler.Plan(pending, 1u << 20, 0, limits);
    ASSERT_EQ(plan.order.size(), 3u);
    EXPECT_EQ(plan.order[0].id, 1u);
    EXPECT_EQ(plan.order[2].id, 3u);
}

} // namespace
