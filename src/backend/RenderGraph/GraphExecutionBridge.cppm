module;

export module VulkanBackend.RenderGraph.GraphExecutionBridge;

export import VulkanBackend.RenderGraph.GraphExecutionContext;
import VulkanBackend.Runtime.FrameLoop;

export namespace VulkanEngine::RenderGraph {

[[nodiscard]] inline GraphExecutionContext CreateGraphExecutionContext(
    const VulkanEngine::Runtime::RuntimeFrameInfo& runtime_frame,
    ImportedFrameResources imported_resources) {
    return GraphExecutionContext{
        .frame_index = runtime_frame.frame_index,
        .swapchain_image_index = runtime_frame.swapchain_image_index,
        .imported_resources = imported_resources,
    };
}

}  // namespace VulkanEngine::RenderGraph

