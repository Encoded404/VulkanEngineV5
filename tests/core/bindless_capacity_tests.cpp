#include <gtest/gtest.h>

import std;

import VulkanEngine.BindlessManager;

namespace {

using VulkanEngine::BindlessManager::BindlessCapacityConfig;
using VulkanEngine::BindlessManager::BindlessCapacityLimits;
using VulkanEngine::BindlessManager::ComputeBindlessCapacity;

TEST(BindlessCapacityTest, AppCapacityWinsWhenItIsTheSmallest) {
    BindlessCapacityConfig config{};
    config.app_capacity = 1024;
    BindlessCapacityLimits limits{};
    limits.max_combined_image_samplers = 1u << 20;
    limits.max_update_after_bind_in_all_pools = 1u << 20;
    EXPECT_EQ(ComputeBindlessCapacity(config, limits), 1024u);
}

TEST(BindlessCapacityTest, ClampedByCombinedImageSamplerLimit) {
    BindlessCapacityConfig config{};
    config.app_capacity = 65536;
    BindlessCapacityLimits limits{};
    limits.max_combined_image_samplers = 4096;
    limits.max_update_after_bind_in_all_pools = 1u << 20;
    EXPECT_EQ(ComputeBindlessCapacity(config, limits), 4096u);
}

TEST(BindlessCapacityTest, ClampedByTheGlobalPoolBudgetMinusOtherPools) {
    BindlessCapacityConfig config{};
    config.app_capacity = 65536;
    config.other_update_after_bind_descriptors = 4096;
    BindlessCapacityLimits limits{};
    limits.max_combined_image_samplers = 1u << 20;
    limits.max_update_after_bind_in_all_pools = 8192;
    // 8192 - 4096 = 4096 available for the bindless array.
    EXPECT_EQ(ComputeBindlessCapacity(config, limits), 4096u);
}

TEST(BindlessCapacityTest, BothClampsApply) {
    BindlessCapacityConfig config{};
    config.app_capacity = 65536;
    config.other_update_after_bind_descriptors = 100;
    BindlessCapacityLimits limits{};
    limits.max_combined_image_samplers = 900;
    limits.max_update_after_bind_in_all_pools = 500;
    // Global budget leaves 400, which is below the 900 combined-image limit.
    EXPECT_EQ(ComputeBindlessCapacity(config, limits), 400u);
}

TEST(BindlessCapacityTest, NeverBelowTheFallbackSlot) {
    BindlessCapacityConfig config{};
    config.app_capacity = 0;
    BindlessCapacityLimits limits{};
    limits.max_combined_image_samplers = 0;
    limits.max_update_after_bind_in_all_pools = 0;
    // Slot 0 must always exist for the fallback.
    EXPECT_EQ(ComputeBindlessCapacity(config, limits), 1u);
}

} // namespace
