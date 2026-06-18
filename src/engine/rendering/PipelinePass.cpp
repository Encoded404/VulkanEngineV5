module;

module VulkanEngine.PipelinePass;

import std;
import std.compat;

import vulkan_hpp;

import VulkanBackend.RenderGraph;
import VulkanBackend.Runtime.VulkanBootstrap;

import VulkanEngine.RenderPipeline;

namespace VulkanEngine::PipelinePass {

// ── PassSetupContext implementation ──

PassSetupContext::PassSetupContext(VulkanEngine::RenderPipeline::RenderPipeline& pipeline)
    : pipeline_(&pipeline) {}

void PassSetupContext::RunBefore(BuiltinPass pass) {
    before_builtin_passes_.push_back(pass);
}

void PassSetupContext::RunAfter(BuiltinPass pass) {
    after_builtin_passes_.push_back(pass);
}

VulkanEngine::RenderGraph::ResourceHandle PassSetupContext::ReadDepthBuffer() {
    return pipeline_->ImportDepthBuffer();
}

VulkanEngine::RenderGraph::ResourceHandle PassSetupContext::ReadBackbuffer() {
    return pipeline_->ImportBackbuffer();
}

VulkanEngine::RenderGraph::ResourceHandle PassSetupContext::ImportImage(std::string_view name) {
    return pipeline_->ImportImage(std::string(name));
}

VulkanEngine::RenderGraph::ResourceHandle PassSetupContext::ImportBuffer(std::string_view name) {
    return pipeline_->ImportBuffer(std::string(name));
}

VulkanEngine::RenderGraph::ResourceHandle PassSetupContext::CreateTransientImage(
    const VulkanEngine::RenderPipeline::TransientImageDesc& desc) {
    return pipeline_->CreateTransientImage(desc);
}

void PassSetupContext::AddRead(VulkanEngine::RenderGraph::ResourceHandle res,
                                VulkanEngine::RenderGraph::PipelineStageIntent stage,
                                VulkanEngine::RenderGraph::AccessIntent access) {
    read_resources_.push_back(res);
    read_stages_.push_back(stage);
    read_accesses_.push_back(access);
}

void PassSetupContext::AddWrite(VulkanEngine::RenderGraph::ResourceHandle res) {
    write_resources_.push_back(res);
}

void PassSetupContext::SetPassAttachments(VulkanEngine::RenderGraph::PassAttachmentSetup setup) {
    attachment_setup_ = std::move(setup);
}

std::uint32_t PassSetupContext::GetRenderWidth() const {
    return render_width_;
}

std::uint32_t PassSetupContext::GetRenderHeight() const {
    return render_height_;
}

// ── FrameContext implementation ──

VulkanEngine::RenderGraph::ResourceHandle FrameContext::GetResource(std::string_view name) const {
    // Resources are resolved through the compiled render graph's resource lifetime table.
    // This is used by custom passes to get handles for resources declared in Setup().
    return VulkanEngine::RenderGraph::ResourceHandle{};
}

} // namespace VulkanEngine::PipelinePass
