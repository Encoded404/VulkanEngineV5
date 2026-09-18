#include <gtest/gtest.h>

import std;

import VulkanEngine.GpuResources.TransientAllocator;

namespace {

using namespace VulkanEngine::GpuResources;

TransientRequirements Req(std::string name,
                          std::uint64_t size,
                          std::int32_t first,
                          std::int32_t last,
                          bool aliasable = true,
                          std::uint32_t heap_key = 0,
                          std::uint64_t alignment = 256) {
    return TransientRequirements{
        .name = std::move(name),
        .kind = TransientKind::Image,
        .heap_key = heap_key,
        .size = size,
        .alignment = alignment,
        .first_pass = first,
        .last_pass = last,
        .aliasable = aliasable,
    };
}

const TransientPlacement* Find(const TransientPlacementPlan& plan, std::uint32_t resource, std::uint32_t slot = 0) {
    for (const auto& placement : plan.placements) {
        if (placement.resource_index == resource && placement.fif_slot == slot) {
            return &placement;
        }
    }
    return nullptr;
}

TEST(TransientAllocatorTest, DisjointLifetimesShareAnOffset) {
    const std::vector<TransientRequirements> requirements{
        Req("a", 256, 0, 1),
        Req("b", 256, 2, 3),
    };

    const auto plan = PlanTransientPlacements(requirements, 1);
    ASSERT_TRUE(plan.valid);
    ASSERT_EQ(plan.placements.size(), 2u);

    const auto* a = Find(plan, 0);
    const auto* b = Find(plan, 1);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(a->offset, 0u);
    EXPECT_EQ(b->offset, 0u);
    EXPECT_EQ(plan.GetHeapCopySize(0), 256u);
    ASSERT_EQ(plan.aliases.size(), 1u);
    EXPECT_EQ(plan.aliases.front().aliased_resource, 1u);
    EXPECT_EQ(plan.aliases.front().after_resource, 0u);
    EXPECT_EQ(plan.aliases.front().pass_index, 2);
}

TEST(TransientAllocatorTest, OverlappingLifetimesDoNotAlias) {
    const std::vector<TransientRequirements> requirements{
        Req("a", 256, 0, 3),
        Req("b", 256, 1, 4),
    };

    const auto plan = PlanTransientPlacements(requirements, 1);
    ASSERT_TRUE(plan.valid);
    const auto* a = Find(plan, 0);
    const auto* b = Find(plan, 1);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_NE(a->offset, b->offset);
    EXPECT_EQ(plan.GetHeapCopySize(0), 512u);
    EXPECT_TRUE(plan.aliases.empty());
}

TEST(TransientAllocatorTest, NonAliasableResourceIsNotReused) {
    const std::vector<TransientRequirements> requirements{
        Req("a", 256, 0, 1, /*aliasable=*/false),
        Req("b", 256, 2, 3, /*aliasable=*/true),
    };

    const auto plan = PlanTransientPlacements(requirements, 1);
    const auto* a = Find(plan, 0);
    const auto* b = Find(plan, 1);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(a->offset, 0u);
    EXPECT_EQ(b->offset, 256u);
    EXPECT_TRUE(plan.aliases.empty());
}

TEST(TransientAllocatorTest, LargerRequirementDoesNotFitSmallerRange) {
    const std::vector<TransientRequirements> requirements{
        Req("a", 256, 0, 1),
        Req("b", 512, 2, 3),
    };

    const auto plan = PlanTransientPlacements(requirements, 1);
    const auto* a = Find(plan, 0);
    const auto* b = Find(plan, 1);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(a->offset, 0u);
    EXPECT_EQ(b->offset, 256u);
    EXPECT_EQ(plan.GetHeapCopySize(0), 768u);
    EXPECT_TRUE(plan.aliases.empty());
}

TEST(TransientAllocatorTest, DifferentHeapKeysAreIndependent) {
    const std::vector<TransientRequirements> requirements{
        Req("a", 256, 0, 1, true, /*heap_key=*/0),
        Req("b", 256, 0, 1, true, /*heap_key=*/1),
    };

    const auto plan = PlanTransientPlacements(requirements, 1);
    const auto* a = Find(plan, 0);
    const auto* b = Find(plan, 1);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(a->heap_key, 0u);
    EXPECT_EQ(b->heap_key, 1u);
    // Both start at 0: separate heaps do not share an address space.
    EXPECT_EQ(a->offset, 0u);
    EXPECT_EQ(b->offset, 0u);
    EXPECT_EQ(plan.GetHeapCopySize(0), 256u);
    EXPECT_EQ(plan.GetHeapCopySize(1), 256u);
}

TEST(TransientAllocatorTest, LayoutIsReplicatedPerFifSlot) {
    const std::vector<TransientRequirements> requirements{
        Req("a", 256, 0, 1),
        Req("b", 256, 2, 3),
    };

    const auto plan = PlanTransientPlacements(requirements, 3);
    ASSERT_EQ(plan.placements.size(), 6u);

    for (std::uint32_t slot = 0; slot < 3; ++slot) {
        const auto* a = Find(plan, 0, slot);
        const auto* b = Find(plan, 1, slot);
        ASSERT_NE(a, nullptr);
        ASSERT_NE(b, nullptr);
        EXPECT_EQ(a->offset, static_cast<std::uint64_t>(slot) * 256u);
        EXPECT_EQ(b->offset, static_cast<std::uint64_t>(slot) * 256u);
    }
}

TEST(TransientAllocatorTest, FreshPlacementRespectsAlignment) {
    const std::vector<TransientRequirements> requirements{
        Req("a", 100, 0, 3, true, 0, 128),
        Req("b", 100, 1, 4, true, 0, 128),
    };

    const auto plan = PlanTransientPlacements(requirements, 1);
    const auto* a = Find(plan, 0);
    const auto* b = Find(plan, 1);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(a->offset % 128, 0u);
    EXPECT_EQ(b->offset % 128, 0u);
    EXPECT_EQ(b->offset, 128u);
}

TEST(TransientAllocatorTest, GranularityPaddingSeparatesLinearAndNonLinear) {
    constexpr std::uint64_t granularity = 512;

    // A non-linear allocation at 0 occupies page 0.
    const std::uint64_t image_offset = 0;
    const std::uint64_t image_size = 100;

    // The next linear allocation must start on a fresh page.
    const std::uint64_t line_offset =
        PadForLinearity(image_offset + image_size, /*previous_is_linear=*/false, /*is_linear=*/true, granularity);
    EXPECT_EQ(line_offset, 512u);
    EXPECT_FALSE(SharesGranularityPage(image_offset, image_size, line_offset, 100, granularity));

    // Two allocations with the same linearity may share a page.
    const std::uint64_t same_linearity = PadForLinearity(0, true, true, granularity);
    EXPECT_EQ(same_linearity, 0u);

    // Crossing a page boundary is detected.
    EXPECT_TRUE(SharesGranularityPage(500, 100, 0, 100, granularity));
}

}  // namespace
