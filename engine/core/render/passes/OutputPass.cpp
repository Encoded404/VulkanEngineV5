module;

#include <logging/logging_macros.hpp>

module VulkanEngine.Render.Passes.OutputPass;

import std;

import logiface;

import vulkan;

import VulkanEngine.PipelinePass;
import VulkanBackend.Vulkan.VulkanBootstrap;

namespace VulkanEngine::SceneRenderer {

namespace {

// Mirrors engine/core/shaders/output.slang. The enum values are deliberately the OutputEncoding ordinals so the shader can branch without a translation table drifting from the backend's definition.
struct OutputPushConstants {
    std::uint32_t encode_mode = 0;
    float exposure = 1.0f;
    float headroom = 1.0f;
    float paper_white_nits = 100.0f;
};
static_assert(sizeof(OutputPushConstants) == 16,
              "output push constants must match the shader layout");

[[nodiscard]] std::uint32_t ShaderEncodeMode(VulkanBackend::Vulkan::OutputEncoding encoding) {
    return static_cast<std::uint32_t>(encoding);
}

} // namespace

OutputPass::OutputPass(VulkanBackend::Vulkan::IVulkanBootstrap* bootstrap,
                       std::uint64_t vertex_shader, std::uint64_t fragment_shader)
    : bootstrap_(bootstrap), vertex_shader_(vertex_shader), fragment_shader_(fragment_shader) {}

OutputPass::~OutputPass() = default;

void OutputPass::Setup(VulkanEngine::PipelinePass::PassSetupContext& ctx) {
    auto scene_color = ctx.ReadSceneColor();
    auto backbuffer = ctx.ReadBackbuffer();
    ctx.AddRead(scene_color, VulkanEngine::RenderGraph::PipelineStageIntent::FragmentShader,
                VulkanEngine::RenderGraph::AccessIntent::Read);
    ctx.AddWrite(backbuffer);

    VulkanEngine::RenderGraph::PassAttachmentSetup setup{};
    setup.auto_begin_rendering = true;
    VulkanEngine::RenderGraph::AttachmentInfo color_attach{};
    color_attach.resource = backbuffer;
    // The full-screen triangle covers every pixel; a clear keeps the image defined even if a driver rejects the draw.
    color_attach.load_op = vk::AttachmentLoadOp::eClear;
    color_attach.store_op = vk::AttachmentStoreOp::eStore;
    color_attach.clear_color = vk::ClearColorValue(std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f});
    setup.color_attachments.push_back(color_attach);
    ctx.SetPassAttachments(setup);

    VulkanEngine::Render::DescriptorDecl decl{};
    decl.set = VulkanEngine::Render::kFirstAppDescriptorSet;
    decl.binding = 0;
    decl.kind = VulkanEngine::Render::DescriptorKind::Shared;
    decl.descriptor_type = vk::DescriptorType::eCombinedImageSampler;
    decl.stage_flags = vk::ShaderStageFlagBits::eFragment;
    decl.count = 1;
    ctx.DeclareBindings({decl});
    ctx.BindResource(VulkanEngine::Render::kFirstAppDescriptorSet, 0, scene_color);

    ctx.DeclarePushConstants<OutputPushConstants>(vk::ShaderStageFlagBits::eFragment);
    ctx.RequestGraphicsPipeline(vertex_shader_, fragment_shader_);
}

void OutputPass::Execute(const VulkanEngine::PipelinePass::FrameContext& ctx,
                         vk::CommandBuffer cmd) {
    if (ctx.pass_pipeline == nullptr || ctx.pipeline_layout == nullptr || bootstrap_ == nullptr) {
        return;
    }

    const auto encoding =
        VulkanBackend::Vulkan::EncodingForSurfaceFormat(bootstrap_->GetSurfaceFormat());
    const auto settings = bootstrap_->GetOutputSettings();

    OutputPushConstants push{};
    push.encode_mode = ShaderEncodeMode(encoding);
    push.exposure = std::max(settings.exposure, 0.0f);
    push.paper_white_nits = std::max(settings.paper_white_nits, 1.0f);
    const float peak = settings.peak_nits > push.paper_white_nits ? settings.peak_nits : 1000.0f;
    push.headroom = (encoding == VulkanBackend::Vulkan::OutputEncoding::Pq ||
                     encoding == VulkanBackend::Vulkan::OutputEncoding::Scrgb)
                        ? peak / push.paper_white_nits
                        : 1.0f;

    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, ctx.pass_pipeline);
    if (!ctx.app_descriptor_sets.empty()) {
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, ctx.pipeline_layout,
                               ctx.first_app_descriptor_set, ctx.app_descriptor_sets, {});
    }
    static_cast<void>(ctx.SetPushConstants(cmd, push));
    cmd.setViewport(0, vk::Viewport(0.0f, 0.0f, static_cast<float>(ctx.render_width),
                                    static_cast<float>(ctx.render_height), 0.0f, 1.0f));
    cmd.setScissor(0, vk::Rect2D{{0, 0}, ctx.render_extent});
    cmd.draw(3, 1, 0, 0);
}

} // namespace VulkanEngine::SceneRenderer
