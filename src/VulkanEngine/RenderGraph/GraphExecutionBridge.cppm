module;

export module VulkanEngine.RenderGraph.GraphExecutionBridge;

export import VulkanEngine.RenderGraph.GraphExecutionContext;
import VulkanEngine.Runtime.RuntimeShell;

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

