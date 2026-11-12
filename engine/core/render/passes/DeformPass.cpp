module;

#include <logging/logging_macros.hpp>

module VulkanEngine.Render.Passes.DeformPass;

import std;
import logiface;
import vulkan;
import VulkanEngine.PipelinePass;
import VulkanEngine.SceneRenderer;

namespace VulkanEngine::SceneRenderer {

DeformPass::DeformPass(SceneRenderer& sr) : scene_renderer_(sr) {}
DeformPass::~DeformPass() = default;

void DeformPass::Setup(VulkanEngine::PipelinePass::PassSetupContext& ctx) {
    auto scene_buffers = ctx.ImportBuffer("scene-buffers");
    auto deform_vertices = ctx.ImportBuffer("deform-vertices");
    // The dispatch list and the SkinEntry/palette buffers are engine-owned
    // per-frame data, not graph resources; "scene-buffers" is the hazard-only
    // token the engine's per-frame buffers travel under.
    ctx.AddRead(scene_buffers, VulkanEngine::RenderGraph::PipelineStageIntent::ComputeShader,
                VulkanEngine::RenderGraph::AccessIntent::Read);
    ctx.AddWrite(deform_vertices);
}

void DeformPass::Execute(const VulkanEngine::PipelinePass::FrameContext& ctx,
                         vk::CommandBuffer cmd) {
    scene_renderer_.DispatchDeform(cmd, ctx.frame_index);
}

} // namespace VulkanEngine::SceneRenderer
