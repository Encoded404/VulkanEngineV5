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
    // The record sizes the layout rows are floored to are engine constants, not types: the renderer pins them to the vk:: types where both are visible.
    EXPECT_EQ(kIndirectCommandBytes, 20u);
    EXPECT_EQ(kSkinEntryBytes, 32u);
}
