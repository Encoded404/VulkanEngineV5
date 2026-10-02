#include <gtest/gtest.h>

import std;

import VulkanEngine.ResidencyPolicy;

namespace {

using VulkanEngine::Residency::EvictionPlan;
using VulkanEngine::Residency::PlanEviction;
using VulkanEngine::Residency::ResidencySet;
using VulkanEngine::Residency::ResidentTexture;

ResidentTexture Make(std::uint64_t id, std::uint64_t bytes, std::uint64_t last_use,
                     bool fallback_bound = false, bool pinned = false) {
    return ResidentTexture{
        .id = id, .bytes = bytes, .last_use_frame = last_use,
        .fallback_bound = fallback_bound, .pinned = pinned,
    };
}

// A set that already fits is never evicted.
TEST(ResidencyPolicyTest, FitsBudgetIsNoOp) {
    const std::array textures{Make(1, 100, 1), Make(2, 200, 2)};
    const EvictionPlan plan = PlanEviction(textures, /*used=*/300, /*budget=*/1000, /*frame=*/5);
    EXPECT_TRUE(plan.evict.empty());
    EXPECT_EQ(plan.resident_bytes, 300u);
}

// Budget 0 disables eviction entirely (the extension-absent default).
TEST(ResidencyPolicyTest, ZeroBudgetDisablesEviction) {
    const std::array textures{Make(1, 100, 1), Make(2, 200, 2)};
    const EvictionPlan plan = PlanEviction(textures, 300, /*budget=*/0, 100);
    EXPECT_TRUE(plan.evict.empty());
}

// Over budget: the least-recently-used texture is evicted first.
TEST(ResidencyPolicyTest, EvictsLeastRecentlyUsedFirst) {
    const std::array textures{Make(1, 100, 30), Make(2, 100, 10), Make(3, 100, 20)};
    // 300 resident, budget 100: must evict two, oldest first (id 2 then 3).
    const EvictionPlan plan = PlanEviction(textures, 300, 100, 40);
    ASSERT_EQ(plan.evict.size(), 2u);
    EXPECT_EQ(plan.evict[0], 2u);
    EXPECT_EQ(plan.evict[1], 3u);
    EXPECT_EQ(plan.resident_bytes, 100u);
    EXPECT_EQ(plan.evicted_bytes, 200u);
}

// The newest texture is kept when only one eviction is needed.
TEST(ResidencyPolicyTest, KeepsMostRecentlyUsed) {
    const std::array textures{Make(1, 100, 1), Make(2, 100, 999)};
    const EvictionPlan plan = PlanEviction(textures, 200, 150, 1000);
    ASSERT_EQ(plan.evict.size(), 1u);
    EXPECT_EQ(plan.evict[0], 1u);
}

// Equal ages break ties deterministically by id.
TEST(ResidencyPolicyTest, TiesBreakById) {
    const std::array textures{Make(7, 100, 5), Make(3, 100, 5), Make(5, 100, 5)};
    const EvictionPlan plan = PlanEviction(textures, 300, 200, 100);
    ASSERT_EQ(plan.evict.size(), 1u);
    EXPECT_EQ(plan.evict[0], 3u);
}

// A pinned texture is never evicted even when it is the oldest.
TEST(ResidencyPolicyTest, PinnedIsNeverEvicted) {
    const std::array textures{Make(1, 100, 1, /*fallback=*/false, /*pinned=*/true),
                              Make(2, 100, 50)};
    const EvictionPlan plan = PlanEviction(textures, 200, 100, 100);
    ASSERT_EQ(plan.evict.size(), 1u);
    EXPECT_EQ(plan.evict[0], 2u);
}

// A fallback-bound reservation has no real binding to evict and is skipped.
TEST(ResidencyPolicyTest, FallbackBoundSkipped) {
    const std::array textures{Make(1, 100, 1, /*fallback=*/true), Make(2, 100, 2)};
    const EvictionPlan plan = PlanEviction(textures, 200, 100, 100);
    ASSERT_EQ(plan.evict.size(), 1u);
    EXPECT_EQ(plan.evict[0], 2u);
}

// A texture younger than the residency floor is not evicted in the same window
// it was uploaded.
TEST(ResidencyPolicyTest, RespectsMinResidencyFrames) {
    const std::array textures{Make(1, 100, 10), Make(2, 100, 9)};
    // current 11, floor 4: ids are only 1..2 frames old, nothing evictable.
    const EvictionPlan plan = PlanEviction(textures, 200, 100, 11, /*min_residency=*/4);
    EXPECT_TRUE(plan.evict.empty());
    // With the floor lifted, the older one goes.
    const EvictionPlan plan2 = PlanEviction(textures, 200, 100, 11, /*min_residency=*/0);
    ASSERT_EQ(plan2.evict.size(), 1u);
    EXPECT_EQ(plan2.evict[0], 2u);
}

// The tracking set reports resident bytes only for committed textures and
// clears the fallback-bound flag on MarkResident.
TEST(ResidencySetTest, TracksAndSizesResidentTextures) {
    ResidencySet set;
    set.Track(1, 0, /*frame=*/0);
    EXPECT_EQ(set.ResidentBytes(), 0u);
    set.MarkResident(1, 512, /*frame=*/1);
    EXPECT_EQ(set.ResidentBytes(), 512u);
    EXPECT_FALSE(set.Get(1)->fallback_bound);
}

// Forget removes the entry so a later plan cannot select it.
TEST(ResidencySetTest, MarkEvictedRemovesEntry) {
    ResidencySet set;
    set.Track(1, 0, 0);
    set.MarkResident(1, 512, 1);
    EXPECT_EQ(set.Size(), 1u);
    set.Track(2, 512, 1);
    set.MarkResident(2, 512, 1);
    const EvictionPlan plan = set.Plan(/*budget=*/512, /*frame=*/100);
    ASSERT_EQ(plan.evict.size(), 1u);
    set.MarkEvicted(plan.evict[0]);
    EXPECT_EQ(set.Size(), 1u);
}

}  // namespace
