module;

#include <logging/logging_macros.hpp>

module VulkanEngine.Render.Passes.OcclusionPass;

import std;
import logiface;
import vulkan_hpp;
import Shaders.Engine.OcclusionCullComp;
import VulkanBackend.Utils.VulkanDebugUtils;
import VulkanEngine.GpuResources;
import VulkanEngine.PipelinePass;
import VulkanEngine.SceneRenderer;

namespace VulkanEngine::SceneRenderer {

static constexpr std::uint32_t MAX_BLOCKS = 1024;

OcclusionPass::OcclusionPass(SceneRenderer& sr) : scene_renderer_(sr) {}
OcclusionPass::~OcclusionPass() { Shutdown(); }

bool OcclusionPass::Create(VulkanBackend::Runtime::IVulkanBootstrap& be) {
    const auto& dev = be.GetDevice();

    {
        std::array<vk::DescriptorSetLayoutBinding, 5> bs{};
        for (std::uint32_t i = 0; i < 3; ++i) {
            bs[i].binding = i;
            bs[i].descriptorType = vk::DescriptorType::eStorageBuffer;
            bs[i].descriptorCount = MAX_BLOCKS;
            bs[i].stageFlags = vk::ShaderStageFlagBits::eCompute;
        }
        bs[3].binding = 3;
        bs[3].descriptorType = vk::DescriptorType::eCombinedImageSampler;
        bs[3].descriptorCount = 1;
        bs[3].stageFlags = vk::ShaderStageFlagBits::eCompute;
        bs[4].binding = 4;
        bs[4].descriptorType = vk::DescriptorType::eStorageBuffer;
        bs[4].descriptorCount = MAX_BLOCKS;
        bs[4].stageFlags = vk::ShaderStageFlagBits::eCompute;
        descriptor_layout_ = std::make_unique<vk::raii::DescriptorSetLayout>(
            dev, vk::DescriptorSetLayoutCreateInfo{{}, static_cast<std::uint32_t>(bs.size()), bs.data()});
        VulkanBackend::Vulkan::SetVulkanObjectName(dev, *descriptor_layout_, "occlusion-layout");

        GpuResources::DescriptorPoolConfig pc{};
        pc.max_sets = 3;
        pc.max_storage_buffers = 3 * MAX_BLOCKS * 4;
        pc.max_sampled_images = 3;
        pc.max_combined_image_samplers = 3;
        descriptor_pool_ = GpuResources::DescriptorPool::Create(be, pc);
        descriptor_pool_->SetDebugName(dev, "occlusion-pool");
    }

    vk::PushConstantRange pr{};
    pr.stageFlags = vk::ShaderStageFlagBits::eCompute;
    pr.size = sizeof(OccPC);
    vk::PipelineLayoutCreateInfo li{};
    li.setLayoutCount = 1;
    li.pSetLayouts = &**descriptor_layout_;
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &pr;
    pipeline_layout_ = std::make_unique<vk::raii::PipelineLayout>(dev, li);
    VulkanBackend::Vulkan::SetVulkanObjectName(dev, *pipeline_layout_, "occlusion-pipeline-layout");

    const vk::raii::ShaderModule mod = Shaders::Engine::OcclusionCullComp::CreateModule(dev);
    const vk::PipelineShaderStageCreateInfo ss({}, vk::ShaderStageFlagBits::eCompute, *mod, "main");
    vk::ComputePipelineCreateInfo ci{};
    ci.stage = ss;
    ci.layout = *pipeline_layout_;
    pipeline_ = std::make_unique<vk::raii::Pipeline>(dev, nullptr, ci);
    VulkanBackend::Vulkan::SetVulkanObjectName(dev, *pipeline_, "occlusion-pipeline");

    LOGIFACE_LOG(debug, "Occlusion pass created");
    return true;
}

void OcclusionPass::Shutdown() {
    pipeline_.reset();
    pipeline_layout_.reset();
    descriptor_pool_.reset();
    descriptor_layout_.reset();
}

void OcclusionPass::Execute(vk::CommandBuffer cmd, vk::DescriptorSet occlusion_set,
                             std::uint32_t entity_count,
                             std::uint32_t hiz_width, std::uint32_t hiz_height) {
    if (!entity_count) {
        LOGIFACE_LOG(debug, "OcclusionPass::Execute: entity_count is 0, skipping");
        return;
    }
    LOGIFACE_LOG(trace, "OcclusionPass::Execute: submeshes=" + std::to_string(entity_count));
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline_);
    const std::array<vk::DescriptorSet, 1> ds{ occlusion_set };
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *pipeline_layout_, 0, ds, {});
    OccPC pc{ entity_count, 1, hiz_width, hiz_height };
    cmd.pushConstants(*pipeline_layout_, vk::ShaderStageFlagBits::eCompute,
                       0, sizeof(OccPC), &pc);
    cmd.dispatch((entity_count + 63) / 64, 1, 1);
}

void OcclusionPass::Setup(VulkanEngine::PipelinePass::PassSetupContext& ctx) {
    auto hiz_image = ctx.ImportImage("hiz-image");
    auto scene_buffers = ctx.ImportBuffer("scene-buffers");
    ctx.AddRead(hiz_image, VulkanEngine::RenderGraph::PipelineStageIntent::ComputeShader,
                VulkanEngine::RenderGraph::AccessIntent::Read);
    ctx.AddRead(scene_buffers, VulkanEngine::RenderGraph::PipelineStageIntent::ComputeShader,
                VulkanEngine::RenderGraph::AccessIntent::Read);
    ctx.AddWrite(scene_buffers);
}

void OcclusionPass::Execute(const VulkanEngine::PipelinePass::FrameContext& ctx,
                             vk::CommandBuffer cmd) {
    scene_renderer_.DispatchOcclusion(cmd, ctx.frame_index);
}

} // namespace VulkanEngine::SceneRenderer
