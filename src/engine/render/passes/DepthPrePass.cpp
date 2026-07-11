module;

#include <logging/logging_macros.hpp>

module VulkanEngine.Render.Passes.DepthPrePass;

import std;
import logiface;
import vulkan_hpp;
import Shaders.Engine.DepthIndirVert;
import Shaders.Engine.DepthPrepassFrag;
import VulkanBackend.Vulkan.VulkanDebugUtils;
import VulkanEngine.PipelinePass;
import VulkanEngine.SceneRenderer;

namespace VulkanEngine::SceneRenderer {

DepthPrePass::DepthPrePass(SceneRenderer& sr) : scene_renderer_(sr) {}
DepthPrePass::~DepthPrePass() { Shutdown(); }

bool DepthPrePass::Create(VulkanBackend::Vulkan::IVulkanBootstrap& be,
                           vk::DescriptorSetLayout empty_layout,
                           vk::DescriptorSetLayout submesh_vertex_layout,
                           vk::DescriptorSetLayout raw_vertex_layout,
                           vk::DescriptorSetLayout indirection_layout,
                           const vk::PipelineRasterizationStateCreateInfo& rs) {
    const auto& dev = be.GetDevice();

    std::array<vk::DescriptorSetLayout, 4> sl{ empty_layout, submesh_vertex_layout, raw_vertex_layout, indirection_layout };
    vk::PipelineLayoutCreateInfo li{};
    li.setLayoutCount = static_cast<std::uint32_t>(sl.size());
    li.pSetLayouts = sl.data();
    pipeline_layout_ = std::make_unique<vk::raii::PipelineLayout>(dev, li);
    VulkanBackend::Vulkan::SetVulkanObjectName(dev, *pipeline_layout_, "depth-prepass-pipeline-layout");

    const vk::raii::ShaderModule vm = Shaders::Engine::DepthIndirVert::CreateModule(dev);
    const vk::raii::ShaderModule fm = Shaders::Engine::DepthPrepassFrag::CreateModule(dev);
    std::array<vk::PipelineShaderStageCreateInfo, 2> ss{
        vk::PipelineShaderStageCreateInfo{{}, vk::ShaderStageFlagBits::eVertex, *vm, "main"},
        vk::PipelineShaderStageCreateInfo{{}, vk::ShaderStageFlagBits::eFragment, *fm, "main"}
    };
    constexpr vk::PipelineVertexInputStateCreateInfo vi({}, 0, nullptr, 0, nullptr);
    constexpr vk::PipelineInputAssemblyStateCreateInfo ia({}, vk::PrimitiveTopology::eTriangleList);
    constexpr vk::PipelineViewportStateCreateInfo vs({}, 1, nullptr, 1, nullptr);
    constexpr vk::PipelineMultisampleStateCreateInfo ms({}, vk::SampleCountFlagBits::e1);
    constexpr vk::PipelineDepthStencilStateCreateInfo ds({}, true, true, vk::CompareOp::eLess);
    constexpr vk::PipelineColorBlendAttachmentState cb{};
    const vk::PipelineColorBlendStateCreateInfo cbs({}, false, vk::LogicOp::eCopy, cb);
    constexpr std::array<vk::DynamicState, 2> dyn{ vk::DynamicState::eViewport, vk::DynamicState::eScissor };
    const vk::PipelineDynamicStateCreateInfo dys({}, dyn);
    vk::PipelineRenderingCreateInfo ri{};
    ri.depthAttachmentFormat = be.GetDepthFormat();
    vk::GraphicsPipelineCreateInfo pi({}, ss, &vi, &ia, nullptr, &vs, &rs, &ms, &ds, &cbs, &dys,
                                       *pipeline_layout_, nullptr, 0, {}, 0);
    pi.setPNext(&ri);
    pipeline_ = std::make_unique<vk::raii::Pipeline>(dev, nullptr, pi);
    VulkanBackend::Vulkan::SetVulkanObjectName(dev, *pipeline_, "depth-prepass-pipeline");

    LOGIFACE_LOG(debug, "Depth prepass created");
    return true;
}

void DepthPrePass::Shutdown() {
    pipeline_.reset();
    pipeline_layout_.reset();
}

void DepthPrePass::Execute(vk::CommandBuffer cmd,
                            std::uint32_t w, std::uint32_t h,
                            vk::DescriptorSet empty_set,
                            vk::DescriptorSet submesh_vertex_set,
                            vk::DescriptorSet bindless_vertex_set,
                            vk::DescriptorSet depth_indirection_set,
                            vk::Buffer draw_count_buffer) {
    if (!pipeline_) {
        LOGIFACE_LOG(warn, "DepthPrePass::Execute: pipeline is null, skipping");
        return;
    }
    LOGIFACE_LOG(trace, "DepthPrePass::Execute: " + std::to_string(w) + "x" + std::to_string(h));
    cmd.setViewport(0, vk::Viewport(0, static_cast<float>(h), static_cast<float>(w),
                                     -static_cast<float>(h), 0, 1));
    cmd.setScissor(0, vk::Rect2D({0, 0}, {w, h}));
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline_);
    const std::array<vk::DescriptorSet, 4> ds{ empty_set, submesh_vertex_set, bindless_vertex_set, depth_indirection_set };
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *pipeline_layout_, 0, ds, {});
    cmd.drawIndirect(draw_count_buffer, 0, 1, sizeof(vk::DrawIndirectCommand));
}

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
