module;

#include <glm/glm.hpp>
#include <logging/logging_macros.hpp>

module VulkanEngine.Render.Passes.ExpandPass;

import std;
import logiface;
import vulkan_hpp;
import Shaders.Engine.ExpandComp;
import VulkanBackend.Utils.VulkanDebugUtils;
import VulkanEngine.GpuResources;
import VulkanEngine.GpuResources.BlockArray;
import VulkanEngine.PipelinePass;
import VulkanEngine.SceneRenderer;

namespace VulkanEngine::SceneRenderer {

ExpandPass::ExpandPass(SceneRenderer& sr) : scene_renderer_(sr) {}
ExpandPass::~ExpandPass() { Shutdown(); }

bool ExpandPass::Create(VulkanBackend::Runtime::IVulkanBootstrap& be,
                         vk::DescriptorSetLayout bindless_index_layout) {
    const auto& dev = be.GetDevice();

    {
        std::array<vk::DescriptorSetLayoutBinding, 6> bs{};
        for (std::uint32_t i = 0; i < 4; ++i) {
            bs[i].binding = i;
            bs[i].descriptorType = vk::DescriptorType::eStorageBuffer;
            bs[i].descriptorCount = SceneRenderer::MAX_BLOCKS;
            bs[i].stageFlags = vk::ShaderStageFlagBits::eCompute;
        }
        for (std::uint32_t i = 4; i < 6; ++i) {
            bs[i].binding = i;
            bs[i].descriptorType = vk::DescriptorType::eStorageBuffer;
            bs[i].descriptorCount = 1;
            bs[i].stageFlags = vk::ShaderStageFlagBits::eCompute;
        }
        descriptor_layout_ = std::make_unique<vk::raii::DescriptorSetLayout>(
            dev, vk::DescriptorSetLayoutCreateInfo{
                {}, static_cast<std::uint32_t>(bs.size()), bs.data() });
        VulkanBackend::Vulkan::SetVulkanObjectName(dev, *descriptor_layout_, "expand-layout");

        GpuResources::DescriptorPoolConfig pc{};
        pc.max_sets = 3;
        pc.max_storage_buffers = 3 * (SceneRenderer::MAX_BLOCKS * 4 + 2);
        descriptor_pool_ = GpuResources::DescriptorPool::Create(be, pc);
        descriptor_pool_->SetDebugName(dev, "expand-pool");
    }

    vk::PushConstantRange pr{};
    pr.stageFlags = vk::ShaderStageFlagBits::eCompute;
    pr.size = sizeof(ExpandPC);
    std::array<vk::DescriptorSetLayout, 2> sl{ *descriptor_layout_, bindless_index_layout };
    vk::PipelineLayoutCreateInfo li{};
    li.setLayoutCount = static_cast<std::uint32_t>(sl.size());
    li.pSetLayouts = sl.data();
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &pr;
    pipeline_layout_ = std::make_unique<vk::raii::PipelineLayout>(dev, li);
    VulkanBackend::Vulkan::SetVulkanObjectName(dev, *pipeline_layout_, "expand-pipeline-layout");

    const vk::raii::ShaderModule mod = Shaders::Engine::ExpandComp::CreateModule(dev);
    const vk::PipelineShaderStageCreateInfo ss({}, vk::ShaderStageFlagBits::eCompute, *mod, "main");
    vk::ComputePipelineCreateInfo ci{};
    ci.stage = ss;
    ci.layout = *pipeline_layout_;
    pipeline_ = std::make_unique<vk::raii::Pipeline>(dev, nullptr, ci);
    VulkanBackend::Vulkan::SetVulkanObjectName(dev, *pipeline_, "expand-pipeline");

    LOGIFACE_LOG(debug, "Expand pass created");
    return true;
}

void ExpandPass::Shutdown() {
    pipeline_.reset();
    pipeline_layout_.reset();
    descriptor_pool_.reset();
    descriptor_layout_.reset();
}

void ExpandPass::Setup(VulkanEngine::PipelinePass::PassSetupContext& ctx) {
    auto scene_buffers = ctx.ImportBuffer("scene-buffers");
    auto draw_indirect = ctx.ImportBuffer("draw-indirect");
    ctx.AddWrite(scene_buffers);
    ctx.AddWrite(draw_indirect);
}

void ExpandPass::Execute(vk::CommandBuffer cmd,
                          vk::DescriptorSet expand_set,
                          vk::DescriptorSet bindless_index_set,
                          std::uint32_t cnt,
                          const glm::mat4& vp) {
    if (!cnt) {
        LOGIFACE_LOG(debug, "ExpandPass::Execute: cnt is 0, skipping");
        return;
    }
    LOGIFACE_LOG(trace, "ExpandPass::Execute: submeshes=" + std::to_string(cnt));
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline_);
    const std::array<vk::DescriptorSet, 2> ds{ expand_set, bindless_index_set };
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *pipeline_layout_,
                             0, ds, {});
    ExpandPC pc{ vp, cnt, 0, 0 };
    cmd.pushConstants(*pipeline_layout_, vk::ShaderStageFlagBits::eCompute,
                       0, sizeof(ExpandPC), &pc);
    cmd.dispatch((cnt + 63) / 64, 1, 1);
}

void ExpandPass::Execute(const VulkanEngine::PipelinePass::FrameContext& ctx,
                          vk::CommandBuffer cmd) {
    const std::uint32_t cnt = scene_renderer_.GetCurrentEntityCount();
    if (cnt == 0) return;
    scene_renderer_.DispatchExpand(cmd, cnt, ctx.view_proj,
                                   ctx.frame_index);
}

} // namespace VulkanEngine::SceneRenderer
