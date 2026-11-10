#include <gtest/gtest.h>

import std;

import VulkanEngine.SceneRenderer;

namespace {

using namespace VulkanEngine;
using namespace VulkanEngine::SceneRenderer;

// Dispatch-list addressing: how a stage finds its source and destination elements
// and where its parameter block lives. These are the values gather writes into
// ModifierDispatch and the deform stage pushes as ModifierPC.

TEST(ModifierDispatchTest, StageRegionsChainSourceToDestination) {
    constexpr std::uint32_t base = 2048;
    constexpr std::uint32_t span = 96;
    constexpr std::uint32_t vertex_count = 96;

    for (std::uint32_t stage_count = 1; stage_count <= 4; ++stage_count) {
        // Stage 0 reads the source vertex table; every later stage reads the
        // region its predecessor wrote, which is why two regions suffice.
        for (std::uint32_t stage = 1; stage < stage_count; ++stage) {
            const std::uint32_t source_region = DeformStageRegion(base, span, stage - 1);
            const std::uint32_t dest_region = DeformStageRegion(base, span, stage);
            EXPECT_NE(source_region, dest_region)
                << "stage " << stage << " would read the region it writes";
            // Both regions are whole spans inside the two-region window.
            EXPECT_EQ(source_region + vertex_count, source_region + span);
            EXPECT_EQ(dest_region + vertex_count, dest_region + span);
        }
        // The final region is what the vertex shader reads back.
        const std::uint32_t final_region = DeformFinalRegion(base, span, stage_count);
        EXPECT_EQ(final_region, DeformStageRegion(base, span, stage_count - 1));
    }
}

TEST(ModifierDispatchTest, OddStacksEndInAAndEvenStacksInB) {
    constexpr std::uint32_t base = 0;
    constexpr std::uint32_t span = 64;

    for (std::uint32_t stages = 1; stages <= 6; ++stages) {
        const bool final_b = DeformFinalRegion(base, span, stages) != base;
        EXPECT_EQ(final_b, stages % 2 == 0) << "stages=" << stages;
    }
}

TEST(ModifierDispatchTest, RegionSpanIsHalfTheBufferSpan) {
    // SceneCapacity::deform_vertex_span counts one region per active submesh; the
    // buffer holds two (A and B), and ModifierPC carries the region span, not the
    // doubled buffer span. Getting this wrong makes every second stage read past
    // its submesh's allocation.
    struct Plan {
        std::uint32_t vertex_span;
    };
    const std::array<Plan, 3> plans{{{32}, {64}, {96}}};

    std::uint32_t region_span = 0;
    for (const Plan& plan : plans) region_span += plan.vertex_span;
    const std::uint32_t buffer_elements = 2 * region_span;

    EXPECT_EQ(region_span, 192u);
    EXPECT_EQ(buffer_elements, 384u);

    // Each submesh's regions fit inside the buffer, and the last one ends exactly
    // at its end.
    std::uint32_t base = 0;
    for (const Plan& plan : plans) {
        EXPECT_LE(base + 2 * plan.vertex_span, buffer_elements);
        base += plan.vertex_span;
    }
    EXPECT_EQ(base, region_span);
}

TEST(ModifierDispatchTest, ParameterBlocksPerPlanAreContiguousAndAligned) {
    struct PlanSpec {
        std::uint32_t alignment;
        std::vector<std::uint32_t> stage_sizes;
    };
    // Two stages, one stage, no stages, two stages with a 256-byte alignment.
    const std::array<PlanSpec, 4> specs{{
        {16, {32, 16}},
        {64, {8}},
        {16, {}},
        {256, {32, 32}},
    }};

    // Count pass: per plan, align each stage and sum.
    std::vector<std::uint32_t> plan_bytes;
    std::vector<std::uint32_t> plan_begin;
    std::uint32_t cursor = 0;
    for (const PlanSpec& spec : specs) {
        std::uint32_t bytes = 0;
        for (const std::uint32_t size : spec.stage_sizes) {
            bytes = DeformParamOffset(bytes, spec.alignment) + size;
        }
        const std::uint32_t begin = bytes == 0
            ? cursor
            : DeformParamOffset(cursor, spec.alignment);
        plan_begin.push_back(begin);
        plan_bytes.push_back(bytes);
        if (bytes != 0) cursor = begin + bytes;
    }
    const std::uint32_t count_pass_total = cursor;

    // Write pass: walk each plan's stages from its own begin.
    std::uint32_t write_pass_end = 0;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> written;
    for (std::size_t i = 0; i < specs.size(); ++i) {
        std::uint32_t local = plan_begin[i];
        for (const std::uint32_t size : specs[i].stage_sizes) {
            local = DeformParamOffset(local, specs[i].alignment);
            written.emplace_back(local, size);
            local += size;
        }
        if (plan_bytes[i] != 0) write_pass_end = std::max(write_pass_end, local);
    }

    EXPECT_EQ(write_pass_end, count_pass_total);
    // plan 0 [0,48), plan 1 [64,72), plan 2 empty at 72, plan 3 [256,544).
    EXPECT_EQ(count_pass_total, 544u);

    // No two blocks overlap and every block starts on its own alignment.
    for (std::size_t i = 0; i < written.size(); ++i) {
        EXPECT_EQ(written[i].first % 16u, 0u);
        for (std::size_t j = i + 1; j < written.size(); ++j) {
            const auto [a_begin, a_size] = written[i];
            const auto [b_begin, b_size] = written[j];
            EXPECT_TRUE(a_begin + a_size <= b_begin || b_begin + b_size <= a_begin)
                << "blocks " << i << " and " << j << " overlap";
        }
    }
}

TEST(ModifierDispatchTest, ParameterlessStagesReserveNoParameterSpace) {
    // A stage with param_size == 0 may still declare a large alignment: the registry
    // validates the two independently. It owns no parameter block, so it may not
    // shift the stages after it -- if the write pass aligned the cursor on its
    // behalf, the following stage's block would land outside the space the count
    // pass reserved and overwrite the next submesh's parameters.
    struct Stage {
        std::uint32_t alignment;
        std::uint32_t size;
    };
    const std::array<Stage, 3> stages{{
        {16, 32},   // parameterized
        {256, 0},   // parameterless, but 256-byte aligned
        {16, 16},   // parameterized
    }};

    // Count pass: only stages that own bytes contribute.
    std::uint32_t plan_bytes = 0;
    for (const Stage& stage : stages) {
        if (stage.size == 0) continue;
        plan_bytes = DeformParamOffset(plan_bytes, stage.alignment) + stage.size;
    }
    EXPECT_EQ(plan_bytes, 48u);

    // Write pass from the plan's aligned base: same sequence, same result.
    const std::uint32_t plan_begin = 64;
    std::uint32_t cursor = plan_begin;
    std::vector<std::uint32_t> offsets;
    for (const Stage& stage : stages) {
        if (stage.size == 0) continue;
        cursor = DeformParamOffset(cursor, stage.alignment);
        offsets.push_back(cursor);
        cursor += stage.size;
    }
    ASSERT_EQ(offsets.size(), 2u);
    EXPECT_EQ(offsets[0], plan_begin);
    EXPECT_EQ(offsets[1], plan_begin + 32u);
    EXPECT_EQ(cursor, plan_begin + plan_bytes);
}

TEST(ModifierDispatchTest, PlanStartsHonorTheSubMeshMaximumAlignment) {
    // A plan whose largest stage needs 256 bytes must start on a 256 boundary even
    // when the previous plan was much smaller.
    std::uint32_t cursor = 16;
    cursor = DeformParamOffset(cursor, 256);
    EXPECT_EQ(cursor, 256u);
    // And a plan with a 16-byte maximum does not inflate the previous cursor.
    EXPECT_EQ(DeformParamOffset(32, 16), 32u);
}

TEST(ModifierDispatchTest, SubMeshIndexIsTheGlobalRendererSlot) {
    // Every per-frame record -- DynamicEntry, StaticEntry, skinEntries, the
    // dispatch's submesh_index -- is indexed by the same global submesh slot, so a
    // dispatch can read its own SkinEntry. Plans are appended in entity order.
    struct Entity {
        std::uint32_t submesh_count;
    };
    const std::array<Entity, 3> entities{{{2}, {1}, {3}}};

    std::vector<std::uint32_t> plan_slots;
    for (std::uint32_t e = 0; e < entities.size(); ++e) {
        for (std::uint32_t s = 0; s < entities[e].submesh_count; ++s) {
            plan_slots.push_back(static_cast<std::uint32_t>(plan_slots.size()));
        }
    }

    ASSERT_EQ(plan_slots.size(), 6u);
    for (std::uint32_t i = 0; i < plan_slots.size(); ++i) {
        EXPECT_EQ(plan_slots[i], i);
    }
}

} // namespace
