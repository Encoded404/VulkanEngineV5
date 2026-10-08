#include <gtest/gtest.h>

import std;

import VulkanBackend.Vulkan.VulkanBootstrap;
import VulkanEngine.GpuResources.BlockArray;
import test_vulkan_fakes;

namespace {

using TestSupport::FakeVulkanBootstrapBackend;
using VulkanEngine::GpuResources::BlockArray;

BlockArray::Config MakeConfig(const std::uint32_t max_blocks) {
    BlockArray::Config cfg{};
    cfg.entry_size = 16;
    cfg.entries_per_block = 256;
    cfg.max_blocks = max_blocks;
    return cfg;
}

} // namespace

TEST(BlockArrayLimitTest, RefusesToGrowPastTheConfiguredLimit) {
    auto backend = std::make_shared<FakeVulkanBootstrapBackend>();
    BlockArray array;
    ASSERT_TRUE(array.Initialize(*backend, MakeConfig(2)));
    EXPECT_EQ(array.BlockLimit(), 2u);
    EXPECT_EQ(array.BlockCount(), 0u);

    // Three blocks are needed but only two are allowed. The limit is checked
    // before any allocation, so the fake backend's throwing accessors are never
    // reached and the array is left untouched.
    EXPECT_FALSE(array.EnsureCapacity(3u * 256u));
    EXPECT_EQ(array.BlockCount(), 0u);
}

TEST(BlockArrayLimitTest, ZeroEntriesSucceedsWithoutAllocating) {
    auto backend = std::make_shared<FakeVulkanBootstrapBackend>();
    BlockArray array;
    ASSERT_TRUE(array.Initialize(*backend, MakeConfig(2)));

    EXPECT_TRUE(array.EnsureCapacity(0));
    EXPECT_EQ(array.BlockCount(), 0u);
}

TEST(BlockArrayLimitTest, UninitializedArrayRefusesCapacity) {
    BlockArray array;
    // No Initialize: EnsureCapacity must report failure rather than crash.
    EXPECT_FALSE(array.EnsureCapacity(1));
}
