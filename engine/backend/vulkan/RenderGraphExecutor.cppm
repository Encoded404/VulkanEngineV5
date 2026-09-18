module;

export module VulkanBackend.Vulkan.RenderGraphExecutor;

import std;

import vulkan_hpp;

import VulkanShared.RenderGraphTypes;

export namespace VulkanBackend::Vulkan {

// Executes a compiled graph using a pre-computed BarrierPlan. The plan carries
// all synchronization; the executor only translates it into synchronization2
// dependency info. No barrier logic lives here.
void ExecuteRenderGraph(const VulkanEngine::RenderGraph::BarrierPlan& plan,
                        const VulkanEngine::RenderGraph::CompiledRenderGraph& graph,
                        const void* user_data,
                        vk::CommandBuffer command_buffer);

} // namespace VulkanBackend::Vulkan
