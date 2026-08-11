module;

#include <logging/logging_macros.hpp>

module VulkanEngine.Render.Passes.DepthPrePass;

import std;
import logiface;
import vulkan_hpp;
import VulkanEngine.PipelinePass;
import VulkanEngine.SceneRenderer;

namespace VulkanEngine::SceneRenderer {

DepthPrePass::DepthPrePass(SceneRenderer& sr) : scene_renderer_(sr) {}
DepthPrePass::~DepthPrePass() = default;

void DepthPrePass::Setup(VulkanEngine::PipelinePass::PassSetupContext& ctx) {
    auto scene_buffers = ctx.ImportBuffer("scene-buffers");
    auto draw_indirect = ctx.ImportBuffer("draw-indirect");
    auto depth_buffer = ctx.ReadDepthBuffer();
    ctx.AddRead(scene_buffers, VulkanEngine::RenderGraph::PipelineStageIntent::VertexShader,
                VulkanEngine::RenderGraph::AccessIntent::Read);
    ctx.AddRead(draw_indirect, VulkanEngine::RenderGraph::PipelineStageIntent::IndirectDraw,
                VulkanEngine::RenderGraph::AccessIntent::Read);
    ctx.AddWrite(depth_buffer);
}

void DepthPrePass::Execute(const VulkanEngine::PipelinePass::FrameContext& ctx,
                            vk::CommandBuffer cmd) {
    scene_renderer_.DepthPrepass(cmd,
        ctx.render_extent.width, ctx.render_extent.height,
        ctx.frame_index);
}

} // namespace VulkanEngine::SceneRenderer
