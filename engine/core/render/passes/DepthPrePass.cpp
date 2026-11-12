module;

#include <logging/logging_macros.hpp>

module VulkanEngine.Render.Passes.DepthPrePass;

import std;
import logiface;
import vulkan;
import VulkanEngine.PipelinePass;
import VulkanEngine.SceneRenderer;

namespace VulkanEngine::SceneRenderer {

DepthPrePass::DepthPrePass(SceneRenderer& sr) : scene_renderer_(sr) {}
DepthPrePass::~DepthPrePass() = default;

void DepthPrePass::Setup(VulkanEngine::PipelinePass::PassSetupContext& ctx) {
    auto scene_buffers = ctx.ImportBuffer("scene-buffers");
    auto draw_indirect = ctx.ImportBuffer("draw-indirect");
    auto depth_indirect = ctx.ImportBuffer("depth-indirect");
    auto depth_buffer = ctx.ReadDepthBuffer();
    ctx.AddRead(scene_buffers, VulkanEngine::RenderGraph::PipelineStageIntent::IndexInput,
                VulkanEngine::RenderGraph::AccessIntent::Read);
    ctx.AddRead(draw_indirect, VulkanEngine::RenderGraph::PipelineStageIntent::IndirectDraw,
                VulkanEngine::RenderGraph::AccessIntent::Read);
    ctx.AddRead(depth_indirect, VulkanEngine::RenderGraph::PipelineStageIntent::IndirectDraw,
                VulkanEngine::RenderGraph::AccessIntent::Read);
    ctx.AddRead(depth_indirect, VulkanEngine::RenderGraph::PipelineStageIntent::IndexInput,
                VulkanEngine::RenderGraph::AccessIntent::Read);
    // The deformed-vertex buffer is a per-FIF engine buffer, not a resource the
    // graph already imports: without this declaration the graph would not
    // insert the deform->draw barrier and the first frame could read the
    // buffer before the deform dispatch completed.
    ctx.AddRead(ctx.ImportBuffer("deform-vertices"),
                VulkanEngine::RenderGraph::PipelineStageIntent::VertexShader,
                VulkanEngine::RenderGraph::AccessIntent::Read);
    ctx.AddWrite(depth_buffer);

    VulkanEngine::RenderGraph::PassAttachmentSetup setup{};
    setup.auto_begin_rendering = true;
    VulkanEngine::RenderGraph::AttachmentInfo depth_attach{};
    depth_attach.resource = depth_buffer;
    depth_attach.load_op = vk::AttachmentLoadOp::eLoad;
    depth_attach.store_op = vk::AttachmentStoreOp::eStore;
    setup.depth_attachment = depth_attach;
    ctx.SetPassAttachments(setup);
}

void DepthPrePass::Execute(const VulkanEngine::PipelinePass::FrameContext& ctx,
                            vk::CommandBuffer cmd) {
    scene_renderer_.DepthPrepass(cmd,
        ctx.render_extent.width, ctx.render_extent.height,
        ctx.frame_index);
}

} // namespace VulkanEngine::SceneRenderer
