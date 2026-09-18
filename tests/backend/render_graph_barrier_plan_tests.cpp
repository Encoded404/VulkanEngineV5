#include <gtest/gtest.h>

import std;

import vulkan_hpp;
import VulkanEngine.RenderGraph;

namespace {

using namespace VulkanEngine::RenderGraph;

// A non-null buffer handle value; never dereferenced, only compared. The pure
// planner only needs identity, so this stands in for a device buffer.
vk::Buffer DummyBuffer(std::uintptr_t value) {
    return vk::Buffer(reinterpret_cast<vk::Buffer::CType>(value));
}

std::uint64_t Fnv1a64(std::string_view text) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char c : text) {
        hash ^= static_cast<std::uint8_t>(c);
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string SerializePlan(const BarrierPlan& plan) {
    std::string out;
    for (const auto& pass : plan.passes) {
        out += pass.pass_name;
        out += ';';
        const auto append_image = [&](const std::vector<PlannedImageBarrier>& barriers, char tag) {
            for (const auto& b : barriers) {
                out += tag;
                out += std::to_string(b.resource_index) + ",";
                out += std::to_string(static_cast<std::uint64_t>(b.src_stage)) + ",";
                out += std::to_string(static_cast<std::uint64_t>(b.dst_stage)) + ",";
                out += std::to_string(static_cast<std::uint64_t>(b.src_access)) + ",";
                out += std::to_string(static_cast<std::uint64_t>(b.dst_access)) + ",";
                out += std::to_string(static_cast<int>(b.old_layout)) + ",";
                out += std::to_string(static_cast<int>(b.new_layout)) + ",";
                out += (b.range.aspectMask & vk::ImageAspectFlagBits::eDepth) != vk::ImageAspectFlags{}
                           ? "depth"
                           : "color";
                out += ";";
            }
        };
        const auto append_buffer = [&](const std::vector<PlannedBufferBarrier>& barriers, char tag) {
            for (const auto& b : barriers) {
                out += tag;
                out += std::to_string(b.resource_index) + ",";
                out += std::to_string(static_cast<std::uint64_t>(b.src_stage)) + ",";
                out += std::to_string(static_cast<std::uint64_t>(b.dst_stage)) + ",";
                out += std::to_string(static_cast<std::uint64_t>(b.src_access)) + ",";
                out += std::to_string(static_cast<std::uint64_t>(b.dst_access)) + ";";
            }
        };
        append_image(pass.pre_image, 'P');
        append_buffer(pass.pre_buffer, 'p');
        append_image(pass.post_image, 'S');
        append_buffer(pass.post_buffer, 's');
    }
    return out;
}

// A representative graph used to pin the planner's output. Mirrors the shapes
// the engine uses: an imported color image with a final Present state, a
// transient image that is written then read, and a transient buffer that is
// read+written in one compute pass.
BarrierPlan BuildRepresentativePlan() {
    RenderGraphBuilder builder;

    const auto backbuffer = builder.ImportResource("swapchain-backbuffer", ResourceKind::Image);
    const auto depth = builder.CreateTransientResource("depth", ResourceKind::Image);
    builder.SetTransientImageInfo(depth, TransientImageInfo{
                                          .format = vk::Format::eD32Sfloat,
                                          .width = 1280,
                                          .height = 720,
                                          .usage = vk::ImageUsageFlagBits::eDepthStencilAttachment |
                                                   vk::ImageUsageFlagBits::eSampled});
    const auto scratch = builder.CreateTransientResource("scratch", ResourceKind::Buffer);

    builder.SetFinalState(backbuffer, ResourceState::ImageState(
                                          PipelineStageIntent::BottomOfPipe,
                                          AccessIntent::None,
                                          QueueType::Graphics,
                                          ImageLayoutIntent::Present));

    const auto depth_pass = builder.AddPass("depth-prepass", QueueType::Graphics, true, {});
    const auto compute_pass = builder.AddPass("compute", QueueType::Graphics, true, {});
    const auto main_pass = builder.AddPass("main", QueueType::Graphics, true, {});

    builder.AddWrite(depth_pass, depth);
    builder.AddRead(compute_pass, depth, PipelineStageIntent::ComputeShader, AccessIntent::Read);
    builder.AddRead(compute_pass, scratch, PipelineStageIntent::ComputeShader, AccessIntent::Read);
    builder.AddWrite(compute_pass, scratch);
    builder.AddRead(main_pass, scratch, PipelineStageIntent::ComputeShader, AccessIntent::Read);
    builder.AddWrite(main_pass, backbuffer);

    builder.AddDependency(depth_pass, compute_pass);
    builder.AddDependency(compute_pass, main_pass);

    const auto graph = builder.Compile();
    EXPECT_TRUE(graph.success);
    return PlanBarriers(graph, ResolvedResourceHandles{}, AliasIntervals{});
}

}  // namespace

TEST(RenderGraphBarrierPlanTest, DeterministicOrderUsesAscendingSlotTieBreak) {
    RenderGraphBuilder builder;

    const auto p0 = builder.AddPass("p0", QueueType::Graphics, true, {});
    builder.AddPass("disabled-1", QueueType::Graphics, false, {});
    builder.AddPass("disabled-2", QueueType::Graphics, false, {});
    const auto p3 = builder.AddPass("p3", QueueType::Graphics, true, {});
    builder.AddPass("disabled-4", QueueType::Graphics, false, {});
    const auto p5 = builder.AddPass("p5", QueueType::Graphics, true, {});

    // p0 unblocks p3; with an index-priority ready set the order is 0,3,5, not
    // 0,5,3 which a FIFO ready queue would produce.
    ASSERT_TRUE(builder.AddDependency(p0, p3));

    const auto result = builder.Compile();
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.passes.size(), 3u);
    EXPECT_EQ(result.passes[0].handle, p0);
    EXPECT_EQ(result.passes[1].handle, p3);
    EXPECT_EQ(result.passes[2].handle, p5);

    // 100 compiles must be byte-identical.
    const std::string first = SerializePlan(PlanBarriers(result, ResolvedResourceHandles{}, AliasIntervals{}));
    for (int i = 0; i < 100; ++i) {
        const auto again = builder.Compile();
        EXPECT_EQ(SerializePlan(PlanBarriers(again, ResolvedResourceHandles{}, AliasIntervals{})), first);
    }
}

TEST(RenderGraphBarrierPlanTest, UndefinedSourceAndPresentUseNoStage) {
    RenderGraphBuilder builder;

    const auto backbuffer = builder.ImportResource("swapchain-backbuffer", ResourceKind::Image);
    builder.SetFinalState(backbuffer, ResourceState::ImageState(
                                           PipelineStageIntent::BottomOfPipe,
                                           AccessIntent::None,
                                           QueueType::Graphics,
                                           ImageLayoutIntent::Present));

    const auto pass = builder.AddPass("main", QueueType::Graphics, true, {});
    ASSERT_TRUE(builder.AddWrite(pass, backbuffer));

    const auto graph = builder.Compile();
    ASSERT_TRUE(graph.success);
    const auto plan = PlanBarriers(graph, ResolvedResourceHandles{}, AliasIntervals{});

    ASSERT_EQ(plan.passes.size(), 1u);
    ASSERT_EQ(plan.passes[0].pre_image.size(), 1u);
    const auto& pre = plan.passes[0].pre_image.front();
    // Undefined source: sync2 uses srcStage=eNone with no access mask.
    EXPECT_EQ(pre.src_stage, vk::PipelineStageFlagBits2::eNone);
    EXPECT_EQ(pre.src_access, vk::AccessFlags2{});
    EXPECT_EQ(pre.old_layout, vk::ImageLayout::eUndefined);

    ASSERT_EQ(plan.passes[0].post_image.size(), 1u);
    const auto& post = plan.passes[0].post_image.front();
    // Present is ordered by the present engine, not a pipeline stage.
    EXPECT_EQ(post.dst_stage, vk::PipelineStageFlagBits2::eNone);
    EXPECT_EQ(post.dst_access, vk::AccessFlags2{});
    EXPECT_EQ(post.new_layout, vk::ImageLayout::ePresentSrcKHR);
}

TEST(RenderGraphBarrierPlanTest, ReadWriteInOnePassCoalescesToASingleTransition) {
    RenderGraphBuilder builder;

    const auto scratch = builder.CreateTransientResource("scratch", ResourceKind::Buffer);
    const auto pass = builder.AddPass("compute", QueueType::Graphics, true, {});

    ASSERT_TRUE(builder.AddRead(pass, scratch, PipelineStageIntent::ComputeShader, AccessIntent::Read));
    ASSERT_TRUE(builder.AddWrite(pass, scratch));

    const auto graph = builder.Compile();
    ASSERT_TRUE(graph.success);

    ASSERT_EQ(graph.passes.size(), 1u);
    ASSERT_EQ(graph.passes[0].pre_pass_transitions.size(), 1u);
    const auto& transition = graph.passes[0].pre_pass_transitions.front();
    EXPECT_EQ(transition.dst_stage, vk::PipelineStageFlagBits2::eComputeShader);
    EXPECT_TRUE((transition.dst_access & vk::AccessFlagBits2::eShaderRead) != vk::AccessFlags2{});
    EXPECT_TRUE((transition.dst_access & vk::AccessFlagBits2::eShaderWrite) != vk::AccessFlags2{});

    const auto plan = PlanBarriers(graph, ResolvedResourceHandles{}, AliasIntervals{});
    ASSERT_EQ(plan.passes[0].pre_buffer.size(), 1u);
}

TEST(RenderGraphBarrierPlanTest, ResolvedBufferProducesScopedBufferBarrier) {
    RenderGraphBuilder builder;

    const auto scratch = builder.CreateTransientResource("scratch", ResourceKind::Buffer);
    const auto pass = builder.AddPass("compute", QueueType::Graphics, true, {});
    ASSERT_TRUE(builder.AddWrite(pass, scratch));

    const auto graph = builder.Compile();
    ASSERT_TRUE(graph.success);

    const vk::Buffer resolved_buffer = DummyBuffer(0x1234);
    ResolvedResourceHandles resolved{};
    resolved.buffers = {resolved_buffer};

    const auto plan = PlanBarriers(graph, resolved, AliasIntervals{});
    ASSERT_EQ(plan.passes.size(), 1u);
    ASSERT_EQ(plan.passes[0].pre_buffer.size(), 1u);
    EXPECT_EQ(plan.passes[0].pre_buffer.front().buffer, resolved_buffer);
}

TEST(RenderGraphBarrierPlanTest, UnresolvedBufferKeepsConservativeFallback) {
    RenderGraphBuilder builder;

    const auto scratch = builder.CreateTransientResource("scratch", ResourceKind::Buffer);
    const auto pass = builder.AddPass("compute", QueueType::Graphics, true, {});
    ASSERT_TRUE(builder.AddWrite(pass, scratch));

    const auto graph = builder.Compile();
    ASSERT_TRUE(graph.success);

    const auto plan = PlanBarriers(graph, ResolvedResourceHandles{}, AliasIntervals{});
    ASSERT_EQ(plan.passes[0].pre_buffer.size(), 1u);
    // Null handle signals the executor to emit a global memory barrier instead
    // of silently dropping the hazard.
    EXPECT_EQ(plan.passes[0].pre_buffer.front().buffer, vk::Buffer{});
}

TEST(RenderGraphBarrierPlanTest, BuilderRejectsInvalidHandlesWithoutIndexing) {
    RenderGraphBuilder builder;

    const auto pass = builder.AddPass("a", QueueType::Graphics, true, {});
    const auto resource = builder.CreateTransientResource("scratch", ResourceKind::Buffer);

    const auto bad_pass = builder.AddWrite(PassHandle{.index = 9999, .generation = 1}, resource);
    ASSERT_FALSE(bad_pass.has_value());
    EXPECT_EQ(bad_pass.error(), GraphBuildError::InvalidPassHandle);

    const auto bad_resource = builder.AddWrite(pass, ResourceHandle{.index = 9999, .generation = 1});
    ASSERT_FALSE(bad_resource.has_value());
    EXPECT_EQ(bad_resource.error(), GraphBuildError::InvalidResourceHandle);

    const auto self = builder.AddDependency(pass, pass);
    ASSERT_FALSE(self.has_value());
    EXPECT_EQ(self.error(), GraphBuildError::SelfDependency);

    // The graph is still intact and compiles.
    EXPECT_TRUE(builder.Compile().success);
}

TEST(RenderGraphBarrierPlanTest, ImportsAreIdempotentByNameAndRebuildIsStable) {
    RenderGraphBuilder builder;

    const auto first = builder.ImportResource("swapchain-backbuffer", ResourceKind::Image);
    const auto second = builder.ImportResource("swapchain-backbuffer", ResourceKind::Image);
    EXPECT_EQ(first, second);

    const auto pass = builder.AddPass("main", QueueType::Graphics, true, {});
    ASSERT_TRUE(builder.AddWrite(pass, first));

    const auto compiled = builder.Compile();
    ASSERT_TRUE(compiled.success);
    ASSERT_EQ(compiled.resource_lifetimes.size(), 1u);

    const std::string plan_before = SerializePlan(PlanBarriers(compiled, ResolvedResourceHandles{}, AliasIntervals{}));

    // Reset and rebuild the same model: identical slots and identical plan.
    builder.Reset();
    const auto rebuilt_backbuffer = builder.ImportResource("swapchain-backbuffer", ResourceKind::Image);
    EXPECT_EQ(rebuilt_backbuffer, first);
    const auto rebuilt_pass = builder.AddPass("main", QueueType::Graphics, true, {});
    ASSERT_TRUE(builder.AddWrite(rebuilt_pass, rebuilt_backbuffer));

    const auto rebuilt = builder.Compile();
    ASSERT_TRUE(rebuilt.success);
    EXPECT_EQ(SerializePlan(PlanBarriers(rebuilt, ResolvedResourceHandles{}, AliasIntervals{})), plan_before);
}

TEST(RenderGraphBarrierPlanTest, GoldenRepresentativePlanSnapshot) {
    const auto plan = BuildRepresentativePlan();
    ASSERT_TRUE(plan.valid);

    const std::uint64_t hash = Fnv1a64(SerializePlan(plan));
    // Pins sync2 scope/layout planning for the representative graph. Update only
    // when the planned synchronization is intentionally changed.
    EXPECT_EQ(hash, 0xad82c6fd845ae51bULL)
        << "golden BarrierPlan hash changed; actual=" << hash
        << " serialized=" << SerializePlan(plan);
}
