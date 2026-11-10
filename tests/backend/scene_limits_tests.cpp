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

TEST(SceneLimitsTest, FixedTablesAreSized) {
    // The skin tables mirror the vertex table: a fixed descriptor array per frame
    // in flight, independent of the block budget.
    EXPECT_EQ(kMaxSkinBuffers, kMaxVertexBuffers);
    EXPECT_EQ(kMaxUvBuffers, kMaxVertexBuffers);
    // The deform set's fixed part: two fully-populated skin arrays (each array
    // element is its own descriptor in the pool) plus the flat palette, the
    // deformed-vertex output and the flat modifier parameters.
    EXPECT_EQ(kDeformPlan.block_bindings, 1u);
    EXPECT_EQ(kDeformPlan.single_storage_buffers, 2u * 64u + 3u);
    // The expand set grew a fifth block array (the SkinEntry table) at binding 7.
    EXPECT_EQ(kExpandPlan.block_bindings, 5u);
    EXPECT_EQ(kExpandPlan.single_storage_buffers, 3u);
}

TEST(SceneLimitsTest, PlanStorageBufferCountsMatchTheirLayouts) {
    constexpr std::uint32_t blocks = 8;

    // Literal expectations, not a copy of StorageBuffersForPlan's formula: a
    // plan row that silently loses a block array changes the production count
    // but not the expectation, so the mismatch is caught here.
    // submesh-vertex: 1 block array + 0 singles.
    EXPECT_EQ(StorageBuffersForPlan(kSubmeshVertexPlan, blocks), 8u);
    // expand: 5 block arrays + 3 singles.
    EXPECT_EQ(StorageBuffersForPlan(kExpandPlan, blocks), 43u);
    // occlusion: 4 block arrays + 4 singles.
    EXPECT_EQ(StorageBuffersForPlan(kOcclusionPlan, blocks), 36u);
    // occluder-select: 3 block arrays + 5 singles.
    EXPECT_EQ(StorageBuffersForPlan(kOccluderSelectPlan, blocks), 29u);
    // collect: 1 block array + 5 singles.
    EXPECT_EQ(StorageBuffersForPlan(kCollectPlan, blocks), 13u);
    // depth-graphics-stage: 1 block array + vertex/UV tables + deformed vertices.
    EXPECT_EQ(StorageBuffersForPlan(kDepthGraphicsStagePlan, blocks), 8u + 64u + 64u + 1u);
    // deform: 1 block array + two skin tables + palette + output + parameters.
    EXPECT_EQ(StorageBuffersForPlan(kDeformPlan, blocks), 8u + 131u);
}

TEST(SceneLimitsTest, WidestPlanBoundsThePerSetTotal) {
    // The deform row is constant and large, so it is the worst case on a small
    // block budget; expand's five block arrays overtake it once the budget passes
    // (131 - 3) / 5 blocks. Both rows matter and neither may be dropped.
    EXPECT_EQ(MaxStorageBuffersPerSet(1u), 132u);     // deform
    EXPECT_EQ(MaxStorageBuffersPerSet(8u), 139u);     // deform
    EXPECT_EQ(MaxStorageBuffersPerSet(256u), 1283u);  // expand: 5 * 256 + 3
    EXPECT_EQ(MaxStorageBuffersPerSet(4096u), 20483u); // expand: 5 * 4096 + 3
}

TEST(SceneLimitsTest, HugeLimitsLeaveTheCeilingInPlace) {
    const DeviceLimits limits{1u << 20, 1u << 20};
    EXPECT_EQ(MaxBlocksForDevice(limits), kMaxBlocksCeiling);
}

TEST(SceneLimitsTest, PerSetLimitClampsTheBudget) {
    // expand needs 5B + 3, so a per-set limit of 4*100 + 4 leaves
    // (404 - 3) / 5 = 80 blocks. This is tighter than occlusion's 4B + 4, which
    // would allow 100: the plan with the most block bindings binds first.
    const DeviceLimits limits{4u * 100u + 4u, 1u << 20};
    EXPECT_EQ(MaxBlocksForDevice(limits), 80u);
}

TEST(SceneLimitsTest, PerStageLimitClampsTheBudget) {
    // Same derivation through maxPerStageDescriptorStorageBuffers.
    const DeviceLimits limits{1u << 20, 4u * 50u + 4u};
    EXPECT_EQ(MaxBlocksForDevice(limits), (204u - 3u) / 5u);
}

TEST(SceneLimitsTest, SkinTablesClampTheBlockBudget) {
    // The deform set's fixed part is 131 storage buffers. With a limit of 131 + 8
    // the depth-graphics-stage row (129 fixed) allows 10 blocks and deform allows
    // 8, so 8 is the answer; neither may be ignored just because it is constant.
    const DeviceLimits limits{131u + 8u, 1u << 20};
    EXPECT_EQ(MaxBlocksForDevice(limits), 8u);
}

TEST(SceneLimitsTest, NeverReturnsZeroBlocks) {
    // A device that cannot fit the fixed bindings still gets one block so the
    // renderer can start and report the condition. A tiny but nonzero limit
    // takes the same floor, so no separate "small device" case is needed.
    const DeviceLimits limits{0, 0};
    EXPECT_EQ(MaxBlocksForDevice(limits), 1u);

    const DeviceLimits tiny{4u + 4u, 4u + 4u};
    EXPECT_EQ(MaxBlocksForDevice(tiny), 1u);

    // Exactly the deform set's fixed part is still not enough to fit one block.
    const DeviceLimits deform_only{131u, 1u << 20};
    EXPECT_EQ(MaxBlocksForDevice(deform_only), 1u);
}

TEST(SceneLimitsTest, EveryPlanIsBudgetedOnce) {
    // One row per SceneRenderer-owned descriptor set. A set that gains bindings
    // without a row would silently consume update-after-bind budget that
    // BindlessManager::ComputeBindlessCapacity does not know about.
    EXPECT_EQ(kSceneSetPlans.size(), 7u);
    for (std::size_t i = 1; i < kSceneSetPlans.size(); ++i) {
        for (std::size_t j = 0; j < i; ++j) {
            EXPECT_STRNE(kSceneSetPlans[i].name.data(), kSceneSetPlans[j].name.data())
                << "duplicate plan row for " << kSceneSetPlans[i].name;
        }
    }
}
