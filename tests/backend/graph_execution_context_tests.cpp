#include <gtest/gtest.h>

import VulkanBackend.RenderGraph;
import VulkanBackend.RenderGraph.GraphExecutionContext;

namespace {

using namespace VulkanEngine::RenderGraph;

TEST(GraphExecutionContextTest, HoldsFrameAndImportedResourceHandles) {
    GraphExecutionContext context{};
    context.frame_index = 2;
    context.swapchain_image_index = 1;
    context.imported_resources.backbuffer = ResourceHandle{.index = 7, .generation = 1};

    EXPECT_EQ(context.frame_index, 2u);
    EXPECT_EQ(context.swapchain_image_index, 1u);
    EXPECT_TRUE(context.imported_resources.backbuffer.IsValid());
    EXPECT_FALSE(context.imported_resources.depth_buffer.IsValid());
}

}  // namespace
