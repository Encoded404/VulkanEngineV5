#include <gtest/gtest.h>

import std;

import vulkan;

import VulkanEngine.SceneGpuLayout;
import VulkanEngine.SceneLimits;

namespace {

using namespace VulkanEngine::SceneGpuLayout;
using namespace VulkanEngine::SceneLimits;

// Binding count each set's layout had when it was hand-written.
// Pinned as literals so a table edit that renumbers a binding (and silently breaks the matching shader) fails here instead of on the GPU.
constexpr std::array<std::pair<SetId, std::uint32_t>, kSetCount> kExpectedBindingCounts{{
    {SetId::submesh_vertex, 1},  {SetId::vertex_buffers, 3}, {SetId::indirection, 1},
    {SetId::index_buffers, 1},   {SetId::deform, 6},         {SetId::expand, 8},
    {SetId::occlusion, 12},      {SetId::occluder_select, 11}, {SetId::collect, 6},
    {SetId::collect_write, 2},   {SetId::empty, 0},          {SetId::scene_uniform, 2},
    {SetId::hiz, 3},
}};

} // namespace

// The structural invariants are static_asserts in the module now, so an edit that violates one fails the build rather than a test run.
// They cover:
// a unique binding number per set.
// one row per slot.
// the block budget only on block arrays.
// every block slot tabled and bound.
// the writer values.
// the fixed-set cardinality.
//
// What is left here pins the numbers the shaders expect and walks the derivations at several block budgets, where a named failure reads better than a compile error.

TEST(SceneTablesTest, SetBindingCountsMatchTheirShaders) {
    for (const auto& [id, expected] : kExpectedBindingCounts) {
        EXPECT_EQ(BindingCount(id), expected) << SetOf(id).name;
    }
}

TEST(SceneTablesTest, EverySetHasExactlyOneRow) {
    // One row per SceneRenderer-owned descriptor set.
    // A set that gains bindings without a row would silently consume update-after-bind budget that BindlessManager::ComputeBindlessCapacity does not know about.
    EXPECT_EQ(kSceneSets.size(), kSetCount);
    for (std::size_t i = 0; i < kSceneSets.size(); ++i) {
        EXPECT_EQ(static_cast<std::size_t>(kSceneSets[i].id), i)
            << "row " << i << " is out of SetId order: " << kSceneSets[i].name;
        for (std::size_t j = 0; j < i; ++j) {
            EXPECT_STRNE(kSceneSets[i].name.data(), kSceneSets[j].name.data())
                << "duplicate set name " << kSceneSets[i].name;
        }
    }
}

TEST(SceneTablesTest, PoolCountsDeriveFromTheRows) {
    constexpr std::uint32_t blocks = 8;
    constexpr std::uint32_t frames = 3;

    // occlusion: 4 block arrays and 7 singles per set, one combined image sampler, three sets in flight.
    const PoolCounts occlusion = PoolCountsFor(SetId::occlusion, blocks, frames);
    EXPECT_EQ(occlusion.max_sets, frames);
    EXPECT_EQ(occlusion.storage_buffers, 39u * frames);
    EXPECT_EQ(occlusion.combined_image_samplers, frames);
    // The occlusion set has no sampled image binding of its own; the Hi-Z image is bound as a combined image sampler.
    // A pool that asked for sampled images here would be reserving descriptors nothing can allocate.
    EXPECT_EQ(occlusion.sampled_images, 0u);
    EXPECT_EQ(occlusion.storage_images, 0u);
    EXPECT_EQ(occlusion.samplers, 0u);

    // hiz: no storage buffers; the storage-image array is kMaxHiZMips wide.
    const PoolCounts hiz = PoolCountsFor(SetId::hiz, blocks, frames);
    EXPECT_EQ(hiz.storage_buffers, 0u);
    EXPECT_EQ(hiz.sampled_images, frames);
    EXPECT_EQ(hiz.samplers, frames);
    EXPECT_EQ(hiz.storage_images, kMaxHiZMips * frames);

    // collect-write: two storage buffers per set, no spare sets.
    const PoolCounts write = PoolCountsFor(SetId::collect_write, blocks, frames);
    EXPECT_EQ(write.max_sets, frames);
    EXPECT_EQ(write.storage_buffers, 2u * frames);

    // Lighting is a single set, not a ring.
    const PoolCounts lighting = PoolCountsFor(SetId::scene_uniform, blocks, frames);
    EXPECT_EQ(lighting.max_sets, 1u);
    EXPECT_EQ(lighting.storage_buffers, 1u + kMaxLightBlocks);

    // The empty set has no descriptor types at all; only its set count matters.
    const PoolCounts empty = PoolCountsFor(SetId::empty, blocks, frames);
    EXPECT_EQ(empty.max_sets, frames);
    EXPECT_EQ(empty.storage_buffers, 0u);
    EXPECT_EQ(empty.combined_image_samplers, 0u);
}

TEST(SceneTablesTest, SetStorageBufferCountsMatchTheirLayouts) {
    constexpr std::uint32_t blocks = 8;

    // Literal expectations, not a copy of the row formula: a set that silently loses a block array changes the production count but not the expectation, so the mismatch is caught here.
    // submesh-vertex: 1 block array + 0 singles.
    EXPECT_EQ(StorageBuffersForSet(SetId::submesh_vertex, blocks), 8u);
    // expand: 5 block arrays + 3 singles.
    EXPECT_EQ(StorageBuffersForSet(SetId::expand, blocks), 43u);
    // occlusion: 4 block arrays + 7 singles.
    EXPECT_EQ(StorageBuffersForSet(SetId::occlusion, blocks), 39u);
    // occluder-select: 3 block arrays + 8 singles.
    EXPECT_EQ(StorageBuffersForSet(SetId::occluder_select, blocks), 32u);
    // collect: 1 block array + 5 singles.
    EXPECT_EQ(StorageBuffersForSet(SetId::collect, blocks), 13u);
    // deform: 1 block array + two skin tables + palette + output + parameters.
    EXPECT_EQ(StorageBuffersForSet(SetId::deform, blocks), 8u + 131u);
    // vertex-buffers: the vertex table, the UV table and the deformed output.
    EXPECT_EQ(StorageBuffersForSet(SetId::vertex_buffers, blocks), 64u + 64u + 1u);
    // index-buffers: the bindless index table.
    EXPECT_EQ(StorageBuffersForSet(SetId::index_buffers, blocks), 64u);
    // indirection: one buffer.
    EXPECT_EQ(StorageBuffersForSet(SetId::indirection, blocks), 1u);
    // collect-write: results + technique commands.
    EXPECT_EQ(StorageBuffersForSet(SetId::collect_write, blocks), 2u);
    // lighting: the scene header plus the light block table.
    EXPECT_EQ(StorageBuffersForSet(SetId::scene_uniform, blocks), 1u + 16u);
    // No storage buffers at all.
    EXPECT_EQ(StorageBuffersForSet(SetId::hiz, blocks), 0u);
    EXPECT_EQ(StorageBuffersForSet(SetId::empty, blocks), 0u);
}

TEST(SceneTablesTest, FixedTablesAreSizedByTheirRows) {
    // The deform set's fixed part: two fully-populated skin arrays (each array element is its own descriptor in the pool) plus the flat palette, the deformed-vertex output and the flat modifier parameters.
    const StorageFit deform = StorageFitOf(SetId::deform);
    EXPECT_EQ(deform.blocks, 1u);
    EXPECT_EQ(deform.fixed, 2u * 64u + 3u);
    // The expand set carries a fifth block array (the SkinEntry table) at binding 7.
    const StorageFit expand = StorageFitOf(SetId::expand);
    EXPECT_EQ(expand.blocks, 5u);
    EXPECT_EQ(expand.fixed, 3u);
}

TEST(SceneTablesTest, DepthGraphicsStageSumsTheThreeSetsItBinds) {
    // The depth/main pipeline binds empty, submesh-vertex, vertex-buffers and indirection on one stage.
    // The per-stage limit applies to their sum, and the sum must be derived from the rows (it used to be restated by hand and was one buffer short: it omitted the indirection set).
    const StorageFit stage = DepthGraphicsStageFit();
    EXPECT_EQ(stage.blocks, 1u);
    EXPECT_EQ(stage.fixed, 64u + 64u + 1u + 1u);
    EXPECT_EQ(stage.blocks * 8u + stage.fixed, 8u + 129u + 1u);
}

TEST(SceneTablesTest, WidestSetBoundsThePerSetTotal) {
    // The deform row is constant and large, so it is the worst case on a small block budget; expand's five block arrays overtake it once the budget passes (131 - 3) / 5 blocks.
    // Both rows matter and neither may be dropped.
    EXPECT_EQ(MaxStorageBuffersPerSet(1u), 132u);      // deform
    EXPECT_EQ(MaxStorageBuffersPerSet(8u), 139u);      // deform
    EXPECT_EQ(MaxStorageBuffersPerSet(256u), 1283u);   // expand: 5 * 256 + 3
    EXPECT_EQ(MaxStorageBuffersPerSet(4096u), 20483u); // expand: 5 * 4096 + 3
}

TEST(SceneTablesTest, HugeLimitsLeaveTheCeilingInPlace) {
    const DeviceLimits limits{1u << 20, 1u << 20};
    EXPECT_EQ(MaxBlocksForDevice(limits), kMaxBlocksCeiling);
}

TEST(SceneTablesTest, PerSetLimitClampsTheBudget) {
    // expand needs 5B + 3, so a per-set limit of 4*100 + 4 leaves (404 - 3) / 5 = 80 blocks.
    // This is tighter than occlusion's 4B + 4, which would allow 100: the set with the most block bindings binds first.
    const DeviceLimits limits{4u * 100u + 4u, 1u << 20};
    EXPECT_EQ(MaxBlocksForDevice(limits), 80u);
}

TEST(SceneTablesTest, PerStageLimitClampsTheBudget) {
    // Same derivation through maxPerStageDescriptorStorageBuffers.
    const DeviceLimits limits{1u << 20, 4u * 50u + 4u};
    EXPECT_EQ(MaxBlocksForDevice(limits), (204u - 3u) / 5u);
}

TEST(SceneTablesTest, SkinTablesClampTheBlockBudget) {
    // The deform set's fixed part is 131 storage buffers.
    // With a limit of 131 + 8 the depth-graphics-stage sum (130 fixed) allows 9 blocks and deform allows 8, so 8 is the answer; neither may be ignored just because it is constant.
    const DeviceLimits limits{131u + 8u, 1u << 20};
    EXPECT_EQ(MaxBlocksForDevice(limits), 8u);
}

TEST(SceneTablesTest, NeverReturnsZeroBlocks) {
    // A device that cannot fit the fixed bindings still gets one block so the renderer can start and report the condition.
    // A tiny but nonzero limit takes the same floor, so no separate "small device" case is needed.
    const DeviceLimits none{0, 0};
    EXPECT_EQ(MaxBlocksForDevice(none), 1u);

    const DeviceLimits tiny{4u + 4u, 4u + 4u};
    EXPECT_EQ(MaxBlocksForDevice(tiny), 1u);

    // Exactly the deform set's fixed part is still not enough to fit one block.
    const DeviceLimits deform_only{131u, 1u << 20};
    EXPECT_EQ(MaxBlocksForDevice(deform_only), 1u);
}

TEST(SceneTablesTest, TableCapacityIsTheRowCount) {
    // The upload paths clamp an array element against these, so a table that changes width changes the bound the upload enforces.
    EXPECT_EQ(TableCapacity(TableId::vertex), kMaxVertexBuffers);
    EXPECT_EQ(TableCapacity(TableId::uv), kMaxUvBuffers);
    EXPECT_EQ(TableCapacity(TableId::index), kMaxIndexBuffers);
    EXPECT_EQ(TableCapacity(TableId::skin), kMaxSkinBuffers);
    EXPECT_EQ(TableCapacity(TableId::skin_extra), kMaxSkinBuffers);
    EXPECT_EQ(TableCapacity(TableId::light), kMaxLightBlocks);
}

TEST(SceneTablesTest, PipelineLayoutsBindTheSetsTheirShadersExpect) {
    // A descriptor layout can be bound at any index, so the set order is the only place the shader's set numbering appears on the CPU side.
    const auto& depth = LayoutOf(PipelineLayoutId::depth).sets;
    ASSERT_EQ(depth.size(), 4u);
    EXPECT_EQ(depth[0], SetId::empty);
    EXPECT_EQ(depth[1], SetId::submesh_vertex);
    EXPECT_EQ(depth[2], SetId::vertex_buffers);
    EXPECT_EQ(depth[3], SetId::indirection);

    const auto& deform = LayoutOf(PipelineLayoutId::deform).sets;
    ASSERT_EQ(deform.size(), 2u);
    EXPECT_EQ(deform[0], SetId::vertex_buffers);
    EXPECT_EQ(deform[1], SetId::deform);

    const auto& expand = LayoutOf(PipelineLayoutId::expand).sets;
    ASSERT_EQ(expand.size(), 2u);
    EXPECT_EQ(expand[0], SetId::expand);
    EXPECT_EQ(expand[1], SetId::index_buffers);

    // Every scene layout except the depth prepass takes a push-constant range; the depth prepass pushes nothing (its coverage push belongs to the technique layouts).
    EXPECT_EQ(LayoutOf(PipelineLayoutId::depth).push_constant_bytes, 0u);
    for (const PipelineLayoutSpec& spec : kPipelineLayouts) {
        if (spec.id == PipelineLayoutId::depth) continue;
        EXPECT_GT(spec.push_constant_bytes, 0u) << spec.name;
    }
}

TEST(SceneTablesTest, EverySceneSetIsBoundByAPipelineLayout) {
    std::array<bool, kSetCount> used{};
    for (const PipelineLayoutSpec& spec : kPipelineLayouts) {
        for (const SetId id : spec.sets) {
            used[static_cast<std::size_t>(id)] = true;
        }
    }
    for (std::size_t i = 0; i < used.size(); ++i) {
        const auto id = static_cast<SetId>(i);
        // The lighting set is bound by the composed technique layouts (DescriptorDecl), not by a scene pipeline layout.
        if (id == SetId::scene_uniform) continue;
        EXPECT_TRUE(used[i]) << SetOf(id).name << " is in no pipeline layout";
    }
}
