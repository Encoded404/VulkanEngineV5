#include <gtest/gtest.h>

#include <vector>

#include <vulkan/vulkan.hpp>

import VulkanBackend.RenderGraph;

namespace {

using namespace VulkanEngine::RenderGraph;

bool ContainsError(const CompiledRenderGraph& result) {
    for (const auto& diagnostic : result.diagnostics) {
        if (diagnostic.severity == DiagnosticSeverity::Error) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST(RenderGraphTest, CompileBuildsDeterministicResourceOrder) {
    RenderGraphBuilder builder;

    const auto backbuffer = builder.ImportResource("backbuffer", ResourceKind::Image);
    const auto lighting = builder.CreateTransientResource("lighting", ResourceKind::Image);
    const auto light_list = builder.CreateTransientResource("light-list", ResourceKind::Buffer);

    const auto depth_pass = builder.AddPass("depth-prepass", QueueType::Graphics, true, {});
    const auto lighting_pass = builder.AddPass("light-culling", QueueType::Compute, true, {});
    const auto shade_pass = builder.AddPass("forward-shade", QueueType::Graphics, true, {});

    EXPECT_TRUE(builder.AddWrite(depth_pass, lighting));
    EXPECT_TRUE(builder.AddRead(lighting_pass, lighting));
    EXPECT_TRUE(builder.AddWrite(lighting_pass, light_list));
    EXPECT_TRUE(builder.AddRead(shade_pass, light_list));
    EXPECT_TRUE(builder.AddRead(shade_pass, lighting));
    EXPECT_TRUE(builder.AddWrite(shade_pass, backbuffer));

    const auto result = builder.Compile();

    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.passes.size(), 3u);
    EXPECT_EQ(result.passes[0].handle, depth_pass);
    EXPECT_EQ(result.passes[1].handle, lighting_pass);
    EXPECT_EQ(result.passes[2].handle, shade_pass);
}

TEST(RenderGraphTest, CompileDetectsExplicitDependencyCycle) {
    RenderGraphBuilder builder;

    const auto pass_a = builder.AddPass("a", QueueType::Graphics, true, {});
    const auto pass_b = builder.AddPass("b", QueueType::Graphics, true, {});

    EXPECT_TRUE(builder.AddDependency(pass_a, pass_b));
    EXPECT_TRUE(builder.AddDependency(pass_b, pass_a));

    const auto result = builder.Compile();

    EXPECT_FALSE(result.success);
    EXPECT_TRUE(ContainsError(result));
}

TEST(RenderGraphTest, DisabledPassesArePrunedFromExecution) {
    RenderGraphBuilder builder;

    const auto image = builder.CreateTransientResource("scratch", ResourceKind::Image);
    const auto enabled_pass = builder.AddPass("enabled", QueueType::Compute, true, {});
    const auto disabled_pass = builder.AddPass("disabled", QueueType::Compute, false, {});

    EXPECT_TRUE(builder.AddWrite(enabled_pass, image));
    EXPECT_TRUE(builder.AddRead(disabled_pass, image));

    const auto result = builder.Compile();

    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.passes.size(), 1u);
    EXPECT_EQ(result.passes.front().handle, enabled_pass);
}

TEST(RenderGraphTest, CompiledGraphExecutesCallbacksInOrder) {
    RenderGraphBuilder builder;
    std::vector<int> call_order{};

    PassExecutionCallback callback_a{};
    callback_a.callback = [&](const void*, vk::CommandBuffer) {
        call_order.push_back(1);
    };

    PassExecutionCallback callback_b{};
    callback_b.callback = [&](const void*, vk::CommandBuffer) {
        call_order.push_back(2);
    };

    const auto pass_a = builder.AddPass("a", QueueType::Graphics, true, callback_a);
    const auto pass_b = builder.AddPass("b", QueueType::Graphics, true, callback_b);

    EXPECT_TRUE(builder.AddDependency(pass_a, pass_b));

    const auto result = builder.Compile();
    ASSERT_TRUE(result.success);

    result.Execute(nullptr, {});

    ASSERT_EQ(call_order.size(), 2u);
    EXPECT_EQ(call_order[0], 1);
    EXPECT_EQ(call_order[1], 2);
}
