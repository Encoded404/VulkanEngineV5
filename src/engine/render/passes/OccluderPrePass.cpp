module;

#include <logging/logging_macros.hpp>

module VulkanEngine.Render.Passes.OccluderPrePass;

import std;
import logiface;
import vulkan_hpp;
import VulkanEngine.PipelinePass;
import VulkanEngine.SceneRenderer;

namespace VulkanEngine::SceneRenderer {

OccluderPrePass::OccluderPrePass(SceneRenderer& sr) : scene_renderer_(sr) {}
OccluderPrePass::~OccluderPrePass() = default;

void OccluderPrePass::Setup(VulkanEngine::PipelinePass::PassSetupContext& ctx) {
    auto scene_buffers = ctx.ImportBuffer("scene-buffers");
    auto occluder_indirect = ctx.ImportBuffer("occluder-indirect");
    auto depth_buffer = ctx.ReadDepthBuffer();
    ctx.AddRead(scene_buffers, VulkanEngine::RenderGraph::PipelineStageIntent::VertexShader,
                VulkanEngine::RenderGraph::AccessIntent::Read);
    ctx.AddRead(occluder_indirect, VulkanEngine::RenderGraph::PipelineStageIntent::IndirectDraw,
                VulkanEngine::RenderGraph::AccessIntent::Read);
    ctx.AddWrite(depth_buffer);
}

void OccluderPrePass::Execute(const VulkanEngine::PipelinePass::FrameContext& ctx,
                                vk::CommandBuffer cmd) {
    scene_renderer_.OccluderPrepass(cmd,
        ctx.render_extent.width, ctx.render_extent.height,
        ctx.frame_index);
}

} // namespace VulkanEngine::SceneRenderer
