module;

#include <cstdint>

export module VulkanBackend.RenderGraph.GraphExecutionContext;

import VulkanBackend.RenderGraph;

export namespace VulkanEngine::RenderGraph {

struct BorrowedCommandContext {
    void* native_command_buffer = nullptr; // NOLINT(misc-non-private-member-variables-in-classes)
};

struct ImportedFrameResources {
    ResourceHandle backbuffer{}; // NOLINT(misc-non-private-member-variables-in-classes)
    ResourceHandle depth_buffer{}; // NOLINT(misc-non-private-member-variables-in-classes)
};

struct GraphExecutionContext {
    uint32_t frame_index = 0; // NOLINT(misc-non-private-member-variables-in-classes)
    uint32_t swapchain_image_index = 0; // NOLINT(misc-non-private-member-variables-in-classes)
    BorrowedCommandContext command_context{}; // NOLINT(misc-non-private-member-variables-in-classes)
    ImportedFrameResources imported_resources{}; // NOLINT(misc-non-private-member-variables-in-classes)
};

}  // namespace VulkanEngine::RenderGraph
