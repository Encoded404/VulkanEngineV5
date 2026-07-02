module;

#include <logging/logging_macros.hpp>

module VulkanEngine.Render.Passes.HiZPass;

import std;
import logiface;
import vulkan_hpp;
import Shaders.Engine.HizGenComp;
import VulkanBackend.Utils.VulkanDebugUtils;
import VulkanEngine.GpuResources;
import VulkanEngine.PipelinePass;

namespace VulkanEngine::SceneRenderer {

HiZPass::~HiZPass() { Shutdown(); }

bool HiZPass::Create(VulkanBackend::Runtime::IVulkanBootstrap& be) {
    const auto& dev = be.GetDevice();

    // Descriptor set layout: 3 bindings (sampled image, sampler, storage images)
    {
        std::array<vk::DescriptorSetLayoutBinding, 3> bs{};
        bs[0].binding = 0;
        bs[0].descriptorType = vk::DescriptorType::eSampledImage;
        bs[0].descriptorCount = 1;
        bs[0].stageFlags = vk::ShaderStageFlagBits::eCompute;
        bs[1].binding = 1;
        bs[1].descriptorType = vk::DescriptorType::eSampler;
        bs[1].descriptorCount = 1;
        bs[1].stageFlags = vk::ShaderStageFlagBits::eCompute;
        bs[2].binding = 2;
        bs[2].descriptorType = vk::DescriptorType::eStorageImage;
        bs[2].descriptorCount = MAX_HIZ_MIPS;
        bs[2].stageFlags = vk::ShaderStageFlagBits::eCompute;
        descriptor_layout_ = std::make_unique<vk::raii::DescriptorSetLayout>(
            dev, vk::DescriptorSetLayoutCreateInfo{{}, static_cast<std::uint32_t>(bs.size()), bs.data()});

        GpuResources::DescriptorPoolConfig pc{};
        pc.max_sets = 3;
        pc.max_storage_images = 3 * MAX_HIZ_MIPS;
        pc.max_sampled_images = 3;
        pc.max_samplers = 3;
        descriptor_pool_ = GpuResources::DescriptorPool::Create(be, pc);
    }

    vk::PushConstantRange pr{};
    pr.stageFlags = vk::ShaderStageFlagBits::eCompute;
    pr.size = sizeof(HiZPC);
    vk::PipelineLayoutCreateInfo li{};
    li.setLayoutCount = 1;
    li.pSetLayouts = &**descriptor_layout_;
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &pr;
    pipeline_layout_ = std::make_unique<vk::raii::PipelineLayout>(dev, li);
    VulkanBackend::Vulkan::SetVulkanObjectName(dev, *pipeline_layout_, "hiz-gen-pipeline-layout");

    const vk::raii::ShaderModule mod = Shaders::Engine::HizGenComp::CreateModule(dev);
    const vk::PipelineShaderStageCreateInfo ss({}, vk::ShaderStageFlagBits::eCompute, *mod, "main");
    vk::ComputePipelineCreateInfo ci{};
    ci.stage = ss;
    ci.layout = *pipeline_layout_;
    pipeline_ = std::make_unique<vk::raii::Pipeline>(dev, nullptr, ci);
    VulkanBackend::Vulkan::SetVulkanObjectName(dev, *pipeline_, "hiz-gen-pipeline");

    LOGIFACE_LOG(debug, "HiZ pass created");
    return true;
}

void HiZPass::Shutdown() {
    pipeline_.reset();
    pipeline_layout_.reset();
    descriptor_pool_.reset();
    descriptor_layout_.reset();
}

void HiZPass::Execute(vk::CommandBuffer cmd,
                       vk::DescriptorSet hiz_set,
                       std::uint32_t w, std::uint32_t h,
                       std::uint32_t mip_count) {
    if (!hiz_set) {
        LOGIFACE_LOG(debug, "HiZPass::Execute: hiz_set is VK_NULL_HANDLE, skipping");
        return;
    }
    LOGIFACE_LOG(trace, "HiZPass::Execute: " + std::to_string(w) + "x" + std::to_string(h) +
                 " mips=" + std::to_string(mip_count));
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline_);
    const std::array<vk::DescriptorSet, 1> ds{ hiz_set };
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *pipeline_layout_, 0, ds, {});

    for (std::uint32_t bl = 0; bl < mip_count - 1; bl += HIZ_BATCH) {
        const std::uint32_t bc = std::min(HIZ_BATCH, mip_count - bl);
        const std::uint32_t sw = w >> bl;
        const std::uint32_t sh = h >> bl;
        HiZPC pc{ bl, sw, sh, bc };
        cmd.pushConstants(*pipeline_layout_, vk::ShaderStageFlagBits::eCompute,
                           0, sizeof(HiZPC), &pc);
        cmd.dispatch((sw + 15) / 16, (sh + 15) / 16, 1);
        vk::MemoryBarrier hiz_mb{};
        hiz_mb.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
        hiz_mb.dstAccessMask = vk::AccessFlagBits::eShaderRead;
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                            vk::PipelineStageFlagBits::eComputeShader,
                            {}, hiz_mb, {}, {});
    }
}

void HiZPass::Setup(VulkanEngine::PipelinePass::PassSetupContext& /*ctx*/) {}

void HiZPass::Execute(const VulkanEngine::PipelinePass::FrameContext& /*ctx*/,
                       vk::CommandBuffer /*cmd*/) {}

} // namespace VulkanEngine::SceneRenderer
