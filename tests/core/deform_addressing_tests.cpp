#include <gtest/gtest.h>

#include <cstddef>

import std;

import VulkanEngine.SceneRenderer;
import VulkanEngine.Mesh.MeshTypes;

namespace {

using namespace VulkanEngine;
using namespace VulkanEngine::SceneRenderer;

// The deform addressing contract: gather re-bases heap element spaces into these
// values, the shaders consume them, and a test can check the arithmetic without a
// device. Every argument below is an element index, never a byte offset.

TEST(DeformAddressingTest, BiasRebasesAbsoluteIndices) {
    // A skin region starting at element 512 serves a window whose absolute source
    // base is 200: table[bias + 200] must name table element 512.
    const std::int32_t bias = DeformBias(512, 200);
    EXPECT_EQ(bias, 312);
    EXPECT_EQ(bias + 200, 512);

    // A region in a later heap block can start below the source base, so the bias
    // is legitimately negative.
    EXPECT_EQ(DeformBias(0, 4096), -4096);
    EXPECT_EQ(DeformBias(4096, 4096), 0);

    // Same region, different submeshes: each window gets its own bias and both
    // still address the same table.
    EXPECT_EQ(DeformBias(512, 200) + 200, DeformBias(512, 300) + 300);
}

TEST(DeformAddressingTest, StageRegionsAlternateBetweenAAndB) {
    constexpr std::uint32_t base = 1024;
    constexpr std::uint32_t span = 64;

    EXPECT_EQ(DeformStageRegion(base, span, 0), base);
    EXPECT_EQ(DeformStageRegion(base, span, 1), base + span);
    EXPECT_EQ(DeformStageRegion(base, span, 2), base);
    EXPECT_EQ(DeformStageRegion(base, span, 3), base + span);

    // Two regions are enough for any 1:1 stack, which is why the deformed-vertex
    // buffer is sized for two spans and not one per stage.
    for (std::uint32_t stage = 0; stage < 16; ++stage) {
        const std::uint32_t region = DeformStageRegion(base, span, stage);
        EXPECT_GE(region, base);
        EXPECT_LT(region, base + 2 * span);
    }
}

TEST(DeformAddressingTest, FinalRegionParityMatchesStackLength) {
    constexpr std::uint32_t base = 256;
    constexpr std::uint32_t span = 32;

    // An even stack ends in B, an odd stack in A; the same parity the gather pass
    // writes into SkinEntry::flags as kFinalRegionB.
    for (std::uint32_t stages = 1; stages <= 5; ++stages) {
        const std::uint32_t expected = (stages % 2 == 0) ? base + span : base;
        EXPECT_EQ(DeformFinalRegion(base, span, stages), expected) << "stages=" << stages;
    }
}

TEST(DeformAddressingTest, VertexDeformBiasNamesTheFinalRegion) {
    // The expand pass writes VertexEntry::deformBias = finalBase - sourceBase and
    // the vertex shader reads deformVertices[deformBias + vertexIndex].
    constexpr std::uint32_t deform_base = 4096;
    constexpr std::uint32_t region_span = 128;
    constexpr std::uint32_t source_base = 900;  // baseVertex + vertexWindowBase
    constexpr std::uint32_t vertex_index = 37;

    // The vertex shader indexes the deform buffer with the *absolute source* index,
    // the same value it would use against the vertex table, so the bias subtracts
    // the base of the source window.
    const std::uint32_t absolute_source_index = source_base + vertex_index;
    for (std::uint32_t stages = 1; stages <= 4; ++stages) {
        const std::uint32_t final_base = DeformFinalRegion(deform_base, region_span, stages);
        const std::int32_t bias = DeformBias(final_base, source_base);
        const std::int32_t read =
            bias + static_cast<std::int32_t>(absolute_source_index);
        EXPECT_EQ(read, static_cast<std::int32_t>(final_base + vertex_index));
        // The read stays inside this submesh's two-region window.
        EXPECT_GE(read, static_cast<std::int32_t>(deform_base));
        EXPECT_LT(read, static_cast<std::int32_t>(deform_base + 2 * region_span));
    }
}

TEST(DeformAddressingTest, NoDeformSentinelCannotCollideWithARealBias) {
    // kNoDeform names "read the source vertex table". It must stay outside every
    // reachable bias: the 24-bit element space limits a real bias well above the
    // most negative int32.
    EXPECT_EQ(kNoDeform, std::numeric_limits<std::int32_t>::min());
    EXPECT_GT(DeformBias(0, (1u << VulkanEngine::SceneRenderer::kEntrySlotShift) - 1u),
              kNoDeform);
}

TEST(DeformAddressingTest, SkinSlotSentinelIsNotAValidArrayIndex) {
    EXPECT_EQ(kNoSkinSlot, 0xFFFFFFFFu);
    // "No skin table" and "no deform bias" are different sentinels in different
    // fields; a shader must never treat one as the other.
    EXPECT_NE(static_cast<std::int64_t>(kNoSkinSlot), static_cast<std::int64_t>(kNoDeform));
}

TEST(DeformAddressingTest, EntryPackingRoundTrips) {
    const std::uint32_t packed = PackEntry(5, 0x00ABCDEFu);
    EXPECT_EQ(EntrySlot(packed), 5u);
    EXPECT_EQ(EntryElement(packed), 0x00ABCDEFu);

    // A slot at the top of the 8-bit field keeps the element intact.
    EXPECT_EQ(EntrySlot(PackEntry(255, 1234)), 255u);
    EXPECT_EQ(EntryElement(PackEntry(255, 1234)), 1234u);

    // The 24-bit element field is the vertex path's hard limit; the boundary value
    // still round-trips.
    constexpr std::uint32_t max_element = (1u << 24) - 1u;
    EXPECT_EQ(PackEntry(0, max_element), max_element);
    EXPECT_EQ(EntryElement(PackEntry(0, max_element)), max_element);
}

TEST(DeformAddressingTest, ParameterOffsetsAlignAndDoNotOverlap) {
    struct Block {
        std::uint32_t alignment;
        std::uint32_t size;
    };
    const std::array<Block, 4> blocks{{
        {16, 32}, {32, 48}, {4, 8}, {256, 64},
    }};

    std::uint32_t cursor = 0;
    std::vector<std::uint32_t> offsets;
    for (const Block& block : blocks) {
        cursor = DeformParamOffset(cursor, block.alignment);
        offsets.push_back(cursor);
        cursor += block.size;
    }

    EXPECT_EQ(offsets[0], 0u);
    EXPECT_EQ(offsets[1], 32u);   // 32 is already 32-aligned
    EXPECT_EQ(offsets[2], 80u);   // 80 is already 16-aligned
    EXPECT_EQ(offsets[3], 256u);  // 88 rounds up to 256
    EXPECT_EQ(cursor, 320u);

    for (std::size_t i = 1; i < offsets.size(); ++i) {
        EXPECT_GE(offsets[i], offsets[i - 1] + blocks[i - 1].size)
            << "block " << i << " overlaps its predecessor";
    }
}

TEST(DeformAddressingTest, ParameterBlocksUseAtLeastSixteenByteAlignment) {
    EXPECT_EQ(DeformParamOffset(0, kDeformParamMinAlignment), 0u);
    EXPECT_EQ(DeformParamOffset(1, 4), 16u);
    EXPECT_EQ(DeformParamOffset(16, 1), 16u);
    EXPECT_EQ(DeformParamOffset(17, 32), 32u);
    EXPECT_EQ(DeformParamOffset(0, 256), 0u);
    EXPECT_EQ(DeformParamOffset(88, 256), 256u);
}

TEST(DeformAddressingTest, InfluenceRecordPacksFourInfluencesIntoThreeWords) {
    BoneWeight weight{};
    weight.bone_indices = {1, 2, 3, 4};
    weight.weights = {255, 0, 128, 1};

    const GpuBoneInfluence gpu = ToGpuInfluence(weight);
    EXPECT_EQ(sizeof(GpuBoneInfluence), 12u);
    EXPECT_EQ(gpu.joints01, 1u | (2u << 16));
    EXPECT_EQ(gpu.joints23, 3u | (4u << 16));
    EXPECT_EQ(gpu.weights, 255u | (0u << 8) | (128u << 16) | (1u << 24));

    // Decode exactly the way skin_layout.slang does.
    EXPECT_EQ(gpu.joints01 & 0xFFFFu, 1u);
    EXPECT_EQ(gpu.joints01 >> 16, 2u);
    EXPECT_EQ(gpu.joints23 & 0xFFFFu, 3u);
    EXPECT_EQ(gpu.joints23 >> 16, 4u);
    EXPECT_EQ(gpu.weights & 0xFFu, 255u);
    EXPECT_EQ((gpu.weights >> 8) & 0xFFu, 0u);
    EXPECT_EQ((gpu.weights >> 16) & 0xFFu, 128u);
    EXPECT_EQ(gpu.weights >> 24, 1u);
}

TEST(DeformAddressingTest, InfluenceRecordSurvivesTheFullJointRange) {
    BoneWeight weight{};
    weight.bone_indices = {0, 65535, 0, 65535};
    weight.weights = {1, 2, 3, 4};

    const GpuBoneInfluence gpu = ToGpuInfluence(weight);
    EXPECT_EQ(gpu.joints01 & 0xFFFFu, 0u);
    EXPECT_EQ(gpu.joints01 >> 16, 65535u);
    EXPECT_EQ(gpu.joints23 & 0xFFFFu, 0u);
    EXPECT_EQ(gpu.joints23 >> 16, 65535u);
    // Joint indices and weights never share a word, so the u16 range cannot leak
    // into a weight.
    EXPECT_EQ(gpu.weights & 0xFFFFu, 1u | (2u << 8));
}

TEST(DeformAddressingTest, SkinEntryLayoutMatchesTheShaderMirror) {
    EXPECT_EQ(sizeof(SkinEntry), 32u);
    EXPECT_EQ(offsetof(SkinEntry, palette_base), 0u);
    EXPECT_EQ(offsetof(SkinEntry, skin_slot), 4u);
    EXPECT_EQ(offsetof(SkinEntry, skin_bias), 8u);
    EXPECT_EQ(offsetof(SkinEntry, extra_slot), 12u);
    EXPECT_EQ(offsetof(SkinEntry, extra_bias), 16u);
    EXPECT_EQ(offsetof(SkinEntry, influence_sets), 20u);
    EXPECT_EQ(offsetof(SkinEntry, flags), 24u);
    EXPECT_EQ(offsetof(SkinEntry, deform_base), 28u);

    // The flag bits are a bitfield shared with the Slang mirror; a collision would
    // silently turn one behaviour into another.
    const std::array<std::uint32_t, 5> flags{
        kDeformActive, kDeformNoOccluder, kDeformNoCullTest, kSkinned, kFinalRegionB};
    for (std::size_t i = 0; i < flags.size(); ++i) {
        for (std::size_t j = i + 1; j < flags.size(); ++j) {
            EXPECT_EQ(flags[i] & flags[j], 0u) << "flag bits " << i << " and " << j << " overlap";
        }
    }
}

TEST(DeformAddressingTest, ModifierPushConstantIsEightWords) {
    EXPECT_EQ(sizeof(ModifierPC), 32u);
    EXPECT_EQ(offsetof(ModifierPC, submesh_index), 0u);
    EXPECT_EQ(offsetof(ModifierPC, stage), 4u);
    EXPECT_EQ(offsetof(ModifierPC, src_slot), 8u);
    EXPECT_EQ(offsetof(ModifierPC, src_base), 12u);
    EXPECT_EQ(offsetof(ModifierPC, vertex_count), 16u);
    EXPECT_EQ(offsetof(ModifierPC, param_offset), 20u);
    EXPECT_EQ(offsetof(ModifierPC, param_size), 24u);
    EXPECT_EQ(offsetof(ModifierPC, region_span), 28u);
}

} // namespace
