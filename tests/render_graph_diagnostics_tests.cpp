#include <gtest/gtest.h>

import VulkanEngine.RenderGraph;

namespace {

using namespace VulkanEngine::RenderGraph;

bool ContainsDiagnosticCode(const CompileResult& result, DiagnosticCode code) {
    for (const auto& diagnostic : result.diagnostics) {
        if (diagnostic.code == code) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST(RenderGraphDiagnosticsTest, EmptyGraphEmitsEmptyGraphDiagnosticCode) {
    RenderGraphBuilder builder;
    const auto result = builder.Compile();

    ASSERT_TRUE(result.success);
    EXPECT_TRUE(ContainsDiagnosticCode(result, DiagnosticCode::EmptyGraph));
}

TEST(RenderGraphDiagnosticsTest, CycleCompileFailureEmitsCycleDiagnosticCode) {
    RenderGraphBuilder builder;

    const auto pass_a = builder.AddPass("a");
    const auto pass_b = builder.AddPass("b");

    ASSERT_TRUE(builder.AddDependency(pass_a, pass_b));
    ASSERT_TRUE(builder.AddDependency(pass_b, pass_a));

    const auto result = builder.Compile();

    ASSERT_FALSE(result.success);
    EXPECT_TRUE(ContainsDiagnosticCode(result, DiagnosticCode::CycleDetected));
}

TEST(RenderGraphDiagnosticsTest, BufferRejectsImageLayoutState) {
    RenderGraphBuilder builder;
    const auto scratch_buffer = builder.CreateTransientResource("scratch", ResourceKind::Buffer);

    const auto invalid_state = ResourceState::ImageState(
        PipelineStageIntent::ComputeShader,
        AccessIntent::Write,
        QueueType::Compute,
        ImageLayoutIntent::General);

    EXPECT_FALSE(builder.SetInitialState(scratch_buffer, invalid_state));

    const auto valid_buffer_state = ResourceState::BufferState(
        PipelineStageIntent::ComputeShader,
        AccessIntent::Write,
        QueueType::Compute);

    EXPECT_TRUE(builder.SetInitialState(scratch_buffer, valid_buffer_state));
    EXPECT_TRUE(builder.SetFinalState(scratch_buffer, valid_buffer_state));
}
