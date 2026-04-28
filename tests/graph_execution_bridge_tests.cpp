#include <gtest/gtest.h>

import VulkanEngine.RenderGraph;
import VulkanEngine.RenderGraph.GraphExecutionBridge;
import VulkanBackend.Runtime.FrameLoop;

namespace {

using namespace VulkanEngine::RenderGraph;
using namespace VulkanEngine::Runtime;

TEST(GraphExecutionBridgeTest, RuntimeFrameMapsToGraphExecutionContext) {
    const RuntimeFrameInfo runtime_frame{
        .frame_index = 4,
        .swapchain_image_index = 1,
        .status = RuntimeStatus::Ok,
    };

    ImportedFrameResources imported{};
    imported.backbuffer = ResourceHandle{.index = 2, .generation = 1};

    const auto graph_context = CreateGraphExecutionContext(runtime_frame, imported);

    EXPECT_EQ(graph_context.frame_index, runtime_frame.frame_index);
    EXPECT_EQ(graph_context.swapchain_image_index, runtime_frame.swapchain_image_index);
    EXPECT_EQ(graph_context.imported_resources.backbuffer, imported.backbuffer);
}

}  // namespace
