module;

module VulkanEngine.Render.Passes.ImGuiPass;

import std;
import vulkan;
import VulkanEngine.PipelinePass;
import VulkanEngine.ImGui;

namespace VulkanEngine::SceneRenderer {

ImGuiPass::ImGuiPass() = default;
ImGuiPass::~ImGuiPass() = default;

void ImGuiPass::Setup(VulkanEngine::PipelinePass::PassSetupContext& ctx) {
    auto scene_color = ctx.ReadSceneColor();
    ctx.AddWrite(scene_color);

    VulkanEngine::RenderGraph::PassAttachmentSetup setup{};
    setup.auto_begin_rendering = false;
    VulkanEngine::RenderGraph::AttachmentInfo color_attach{};
    color_attach.resource = scene_color;
    color_attach.load_op = vk::AttachmentLoadOp::eLoad;
    color_attach.store_op = vk::AttachmentStoreOp::eStore;
    setup.color_attachments.push_back(color_attach);
    ctx.SetPassAttachments(setup);
}

void ImGuiPass::Execute(const VulkanEngine::PipelinePass::FrameContext& ctx,
                        vk::CommandBuffer cmd) {
    if (ctx.imgui == nullptr || !ctx.imgui->IsInitialized()) {
        return;
    }
    const auto scene = ctx.GetResource("scene-color");
    if (!scene.has_value() || scene->AsImageView() == nullptr) {
        return;
    }
    ctx.imgui->RenderDrawData(cmd, scene->AsImageView(), ctx.render_width, ctx.render_height);
}

} // namespace VulkanEngine::SceneRenderer
