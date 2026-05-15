module;

#include <cstdint>
#include <vector>

#include <vulkan/vulkan.hpp>

module VulkanEngine.FrameRenderer;

import VulkanEngine.Runtime.VulkanBootstrap;
import VulkanEngine.RenderPipeline;
import VulkanEngine.ImGuiSystem;
import VulkanEngine.RenderGraph;

namespace VulkanEngine::FrameRenderer {

void FrameRenderer::Initialize(VulkanEngine::Runtime::VulkanBootstrap& bootstrap,
                               VulkanEngine::RenderPipeline::RenderPipeline& pipeline,
                               const FrameRendererConfig& config) {
    bootstrap_ = &bootstrap;
    enable_imgui_ = config.enable_imgui;
    const uint32_t swapchain_image_count = bootstrap.GetSnapshot().swapchain_image_count;
    swapchain_image_presented_.resize(swapchain_image_count, false);

    const auto& compiled_graph = pipeline.GetCompiledGraph();
    for (size_t i = 0; i < compiled_graph.resource_lifetimes.size(); ++i) {
        const auto& resource = compiled_graph.resource_lifetimes[i];
        if (resource.imported) {
            if (resource.name == "swapchain-backbuffer") {
                backbuffer_resource_index_ = static_cast<uint32_t>(i);
            } else if (resource.name == "depth-buffer") {
                depth_buffer_resource_index_ = static_cast<uint32_t>(i);
            }
        }
    }
}

void FrameRenderer::Shutdown() {
    bootstrap_ = nullptr;
    swapchain_image_presented_.clear();
    depth_buffer_initialized_ = false;
}

void FrameRenderer::RenderFrame(VulkanEngine::RenderPipeline::RenderPipeline& pipeline,
                                const void* pass_user_data,
                                VulkanEngine::ImGuiSystem::ImGuiSystem* imgui,
                                uint32_t image_index) {
    if (!pipeline.IsCompiled()) {
        return;
    }

    auto& backend = bootstrap_->GetBackend();
    const uint32_t frame_idx = bootstrap_->GetSnapshot().frame_index;
    auto& cmd = backend.GetCommandBuffer(frame_idx);
    cmd.reset({});
    cmd.begin({vk::CommandBufferUsageFlagBits::eOneTimeSubmit});

    const uint32_t swapchain_image_count = bootstrap_->GetSnapshot().swapchain_image_count;
    if (swapchain_image_presented_.size() < swapchain_image_count) {
        swapchain_image_presented_.resize(swapchain_image_count, false);
    }

    const bool image_was_presented = swapchain_image_presented_[image_index];

    pipeline.SetImportedResourceState(backbuffer_resource_index_,
        VulkanEngine::RenderGraph::ResourceState::ImageState(
            image_was_presented
                ? VulkanEngine::RenderGraph::PipelineStageIntent::BottomOfPipe
                : VulkanEngine::RenderGraph::PipelineStageIntent::TopOfPipe,
            VulkanEngine::RenderGraph::AccessIntent::None,
            VulkanEngine::RenderGraph::QueueType::Graphics,
            image_was_presented
                ? VulkanEngine::RenderGraph::ImageLayoutIntent::Present
                : VulkanEngine::RenderGraph::ImageLayoutIntent::Undefined));

    if (depth_buffer_initialized_) {
        pipeline.SetImportedResourceState(depth_buffer_resource_index_,
            VulkanEngine::RenderGraph::ResourceState::ImageState(
                VulkanEngine::RenderGraph::PipelineStageIntent::DepthAttachment,
                VulkanEngine::RenderGraph::AccessIntent::Write,
                VulkanEngine::RenderGraph::QueueType::Graphics,
                VulkanEngine::RenderGraph::ImageLayoutIntent::DepthAttachment));
    }

    pipeline.Execute(pass_user_data, cmd, image_index);

    swapchain_image_presented_[image_index] = true;
    depth_buffer_initialized_ = true;

    if (enable_imgui_ && imgui && imgui->IsInitialized()) {
        uint32_t w = 0, h = 0;
        if (backend.GetSwapchainExtent(w, h) && w > 0 && h > 0) {
            imgui->RenderDrawData(cmd, *backend.GetSwapchainImageViews()[image_index], w, h);
        }
    }

    {
        vk::ImageMemoryBarrier present_barrier{};
        present_barrier.image = backend.GetSwapchainImages()[image_index];
        present_barrier.srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
        present_barrier.dstAccessMask = {};
        present_barrier.oldLayout = vk::ImageLayout::eColorAttachmentOptimal;
        present_barrier.newLayout = vk::ImageLayout::ePresentSrcKHR;
        present_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        present_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        present_barrier.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};

        cmd.pipelineBarrier(
            vk::PipelineStageFlagBits::eColorAttachmentOutput,
            vk::PipelineStageFlagBits::eBottomOfPipe,
            {}, {}, {}, present_barrier);
    }

    cmd.end();
}

} // namespace VulkanEngine::FrameRenderer
