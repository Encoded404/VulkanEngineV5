module;

export module VulkanBackend.Vulkan.GraphExecutionBridge;

export import VulkanBackend.Vulkan.GraphExecutionContext;
import VulkanBackend.Vulkan.FrameLoop;

export namespace VulkanEngine::RenderGraph {

[[nodiscard]] inline GraphExecutionContext CreateGraphExecutionContext(
    const VulkanBackend::Vulkan::RuntimeFrameInfo& runtime_frame,
    ImportedFrameResources imported_resources) {
    return GraphExecutionContext{
        .frame_index = runtime_frame.frame_index,
        .swapchain_image_index = runtime_frame.swapchain_image_index,
        .imported_resources = imported_resources,
    };
}

}  // namespace VulkanEngine::RenderGraph

