#include <gtest/gtest.h>

import std;

import VulkanEngine.SceneLimits;

namespace {

using namespace VulkanEngine::SceneLimits;

} // namespace

TEST(SceneLimitsTest, EntriesPerBlockMirrorsTheShaderConstant) {
    // The Slang mirror is shaders/scene_block_layout.slang. If this changes, the
    // seven submesh shaders must change with it.
    EXPECT_EQ(kEntriesPerBlock, 256u);
    EXPECT_EQ(kMaxSubmeshesCeiling, kMaxBlocksCeiling * kEntriesPerBlock);
}

TEST(SceneLimitsTest, PlanStorageBufferCountsMatchTheirLayouts) {
    constexpr std::uint32_t blocks = 8;

    // Literal expectations, not a copy of StorageBuffersForPlan's formula: a
    // plan row that silently loses a block array changes the production count
    // but not the expectation, so the mismatch is caught here.
    // submesh-vertex: 1 block array + 0 singles.
    EXPECT_EQ(StorageBuffersForPlan(kSubmeshVertexPlan, blocks), 8u);
    // expand: 4 block arrays + 3 singles.
    EXPECT_EQ(StorageBuffersForPlan(kExpandPlan, blocks), 35u);
    // occlusion: 4 block arrays + 4 singles.
    EXPECT_EQ(StorageBuffersForPlan(kOcclusionPlan, blocks), 36u);
    // occluder-select: 3 block arrays + 5 singles.
    EXPECT_EQ(StorageBuffersForPlan(kOccluderSelectPlan, blocks), 29u);
    // collect: 1 block array + 5 singles.
    EXPECT_EQ(StorageBuffersForPlan(kCollectPlan, blocks), 13u);
}

TEST(SceneLimitsTest, HugeLimitsLeaveTheCeilingInPlace) {
    const DeviceLimits limits{1u << 20, 1u << 20};
    EXPECT_EQ(MaxBlocksForDevice(limits), kMaxBlocksCeiling);
}

TEST(SceneLimitsTest, PerSetLimitClampsTheBudget) {
    // occlusion needs 4B + 4; with a per-set limit of 4*100 + 4 the largest
    // fitting block count is 100.
    const DeviceLimits limits{4u * 100u + 4u, 1u << 20};
    EXPECT_EQ(MaxBlocksForDevice(limits), 100u);
}

TEST(SceneLimitsTest, PerStageLimitClampsTheBudget) {
    // Same derivation through maxPerStageDescriptorStorageBuffers.
    const DeviceLimits limits{1u << 20, 4u * 50u + 4u};
    EXPECT_EQ(MaxBlocksForDevice(limits), 50u);
}

TEST(SceneLimitsTest, NeverReturnsZeroBlocks) {
    // A device that cannot fit the fixed bindings still gets one block so the
    // renderer can start and report the condition. A tiny but nonzero limit
    // takes the same floor, so no separate "small device" case is needed.
    const DeviceLimits limits{0, 0};
    EXPECT_EQ(MaxBlocksForDevice(limits), 1u);

    const DeviceLimits tiny{4u + 4u, 4u + 4u};
    EXPECT_EQ(MaxBlocksForDevice(tiny), 1u);
}
