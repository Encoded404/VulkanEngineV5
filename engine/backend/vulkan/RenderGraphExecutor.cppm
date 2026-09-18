module;

export module VulkanBackend.Vulkan.RenderGraphExecutor;

import std;

import vulkan_hpp;

import VulkanShared.RenderGraphTypes;

export namespace VulkanBackend::Vulkan {

void ExecuteRenderGraph(const VulkanEngine::RenderGraph::CompiledRenderGraph& graph,
                        const void* user_data,
                        vk::CommandBuffer command_buffer);

} // namespace VulkanBackend::Vulkan
