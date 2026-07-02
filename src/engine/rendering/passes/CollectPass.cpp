module;

#include <logging/logging_macros.hpp>

module VulkanEngine.Render.Passes.CollectPass;

import std;
import logiface;
import vulkan_hpp;
import Shaders.Engine.CollectCountCompactComp;
import Shaders.Engine.CollectWriteComp;
import VulkanBackend.Utils.VulkanDebugUtils;
import VulkanEngine.GpuResources;
import VulkanEngine.GpuResources.BlockArray;
import VulkanEngine.PipelinePass;

namespace VulkanEngine::SceneRenderer {

namespace {
    void WriteBlocks(vk::DescriptorSet ds, std::uint32_t binding,
                     GpuResources::BlockArray& buf,
                     vk::DescriptorType desc_type,
                     const vk::raii::Device& dev) {
        for (std::uint32_t bi = 0; bi < buf.BlockCount(); ++bi) {
            const vk::DescriptorBufferInfo bii(buf.GetBlockArray(bi), 0, buf.BlockSize());
            vk::WriteDescriptorSet w{};
            w.dstSet = ds; w.dstBinding = binding; w.dstArrayElement = bi;
            w.descriptorCount = 1; w.descriptorType = desc_type; w.pBufferInfo = &bii;
            dev.updateDescriptorSets(w, nullptr);
        }
    }
}

CollectPass::~CollectPass() { Shutdown(); }

bool CollectPass::Create(VulkanBackend::Runtime::IVulkanBootstrap& be) {
    const auto& dev = be.GetDevice();
    static constexpr std::uint32_t FRAMES_IN_FLIGHT = 3;

    // Count + compact layout (4 bindings)
    {
        std::array<vk::DescriptorSetLayoutBinding, 4> bs{};
        bs[0].binding = 0; bs[0].descriptorType = vk::DescriptorType::eStorageBuffer;
        bs[0].descriptorCount = MAX_BLOCKS; bs[0].stageFlags = vk::ShaderStageFlagBits::eCompute;
        for (std::uint32_t i = 1; i < 4; ++i) {
            bs[i].binding = i; bs[i].descriptorType = vk::DescriptorType::eStorageBuffer;
            bs[i].descriptorCount = 1; bs[i].stageFlags = vk::ShaderStageFlagBits::eCompute;
        }
        collect_layout_ = std::make_unique<vk::raii::DescriptorSetLayout>(
            dev, vk::DescriptorSetLayoutCreateInfo{{}, static_cast<std::uint32_t>(bs.size()), bs.data()});
        VulkanBackend::Vulkan::SetVulkanObjectName(dev, *collect_layout_, "collect-layout");
        GpuResources::DescriptorPoolConfig pc{};
        pc.max_sets = FRAMES_IN_FLIGHT; pc.max_storage_buffers = FRAMES_IN_FLIGHT * (MAX_BLOCKS + 3);
        collect_pool_ = GpuResources::DescriptorPool::Create(be, pc);
        collect_pool_->SetDebugName(dev, "collect-pool");
    }

    // Write layout (4 bindings)
    {
        std::array<vk::DescriptorSetLayoutBinding, 4> bs{};
        for (std::uint32_t i = 0; i < 4; ++i) {
            bs[i].binding = i; bs[i].descriptorType = vk::DescriptorType::eStorageBuffer;
            bs[i].descriptorCount = 1; bs[i].stageFlags = vk::ShaderStageFlagBits::eCompute;
        }
        write_layout_ = std::make_unique<vk::raii::DescriptorSetLayout>(
            dev, vk::DescriptorSetLayoutCreateInfo{{}, static_cast<std::uint32_t>(bs.size()), bs.data()});
        VulkanBackend::Vulkan::SetVulkanObjectName(dev, *write_layout_, "collect-write-layout");
        GpuResources::DescriptorPoolConfig pc{};
        pc.max_sets = FRAMES_IN_FLIGHT + 1; pc.max_storage_buffers = FRAMES_IN_FLIGHT * 5;
        write_pool_ = GpuResources::DescriptorPool::Create(be, pc);
        write_pool_->SetDebugName(dev, "collect-write-pool");
    }

    // Count/compact pipeline
    {
        vk::PushConstantRange pr{};
        pr.stageFlags = vk::ShaderStageFlagBits::eCompute; pr.size = sizeof(CollectPC);
        vk::PipelineLayoutCreateInfo li{};
        li.setLayoutCount = 1; li.pSetLayouts = &**collect_layout_;
        li.pushConstantRangeCount = 1; li.pPushConstantRanges = &pr;
        count_pipeline_layout_ = std::make_unique<vk::raii::PipelineLayout>(dev, li);
        VulkanBackend::Vulkan::SetVulkanObjectName(dev, *count_pipeline_layout_, "collect-count-pipeline-layout");
        const vk::raii::ShaderModule mod = Shaders::Engine::CollectCountCompactComp::CreateModule(dev);
        const vk::PipelineShaderStageCreateInfo ss({}, vk::ShaderStageFlagBits::eCompute, *mod, "main");
        vk::ComputePipelineCreateInfo ci{}; ci.stage = ss; ci.layout = *count_pipeline_layout_;
        count_pipeline_ = std::make_unique<vk::raii::Pipeline>(dev, nullptr, ci);
        VulkanBackend::Vulkan::SetVulkanObjectName(dev, *count_pipeline_, "collect-count-pipeline");
    }

    // Write pipeline
    {
        vk::PushConstantRange pr{};
        pr.stageFlags = vk::ShaderStageFlagBits::eCompute; pr.size = sizeof(WritePC);
        vk::PipelineLayoutCreateInfo li{};
        li.setLayoutCount = 1; li.pSetLayouts = &**write_layout_;
        li.pushConstantRangeCount = 1; li.pPushConstantRanges = &pr;
        write_pipeline_layout_ = std::make_unique<vk::raii::PipelineLayout>(dev, li);
        VulkanBackend::Vulkan::SetVulkanObjectName(dev, *write_pipeline_layout_, "collect-write-pipeline-layout");
        const vk::raii::ShaderModule mod = Shaders::Engine::CollectWriteComp::CreateModule(dev);
        const vk::PipelineShaderStageCreateInfo ss({}, vk::ShaderStageFlagBits::eCompute, *mod, "main");
        vk::ComputePipelineCreateInfo ci{}; ci.stage = ss; ci.layout = *write_pipeline_layout_;
        write_pipeline_ = std::make_unique<vk::raii::Pipeline>(dev, nullptr, ci);
        VulkanBackend::Vulkan::SetVulkanObjectName(dev, *write_pipeline_, "collect-write-pipeline");
    }

    LOGIFACE_LOG(debug, "Collect pass created");
    return true;
}

void CollectPass::Shutdown() {
    count_pipeline_.reset(); count_pipeline_layout_.reset();
    write_pipeline_.reset(); write_pipeline_layout_.reset();
    collect_pool_.reset(); collect_layout_.reset();
    write_pool_.reset(); write_layout_.reset();
}

void CollectPass::Execute(vk::CommandBuffer cmd,
                           vk::DescriptorSet collect_set,
                           vk::DescriptorSet collect_write_set,
                           std::uint32_t entity_count,
                           const vk::raii::Device& dev,
                           GpuResources::GpuBuffer& intermediate_buffer,
                           GpuResources::GpuBuffer& tech_counts_buffer,
                           GpuResources::GpuBuffer& tech_offsets_buffer,
                           GpuResources::GpuBuffer& technique_draw_commands,
                           GpuResources::GpuBuffer& indirection_buffer,
                           GpuResources::GpuBuffer& compacted_indirection_buffer,
                           GpuResources::BlockArray& submesh_cull) {
    if (!entity_count) {
        LOGIFACE_LOG(debug, "CollectPass::Execute: entity_count is 0, skipping");
        return;
    }
    LOGIFACE_LOG(trace, "CollectPass::Execute: submeshes=" + std::to_string(entity_count) +
                 " techniques=" + std::to_string(MAX_TECHNIQUES));

    // Write collect set bindings
    WriteBlocks(collect_set, 0, submesh_cull, vk::DescriptorType::eStorageBuffer, dev);
    {
        const vk::DescriptorBufferInfo bi(*indirection_buffer.GetBuffer(), 0, vk::WholeSize);
        vk::WriteDescriptorSet w{};
        w.dstSet = collect_set; w.dstBinding = 1; w.descriptorCount = 1;
        w.descriptorType = vk::DescriptorType::eStorageBuffer; w.pBufferInfo = &bi;
        dev.updateDescriptorSets(w, nullptr);
    }
    {
        const vk::DescriptorBufferInfo bi(*compacted_indirection_buffer.GetBuffer(), 0, vk::WholeSize);
        vk::WriteDescriptorSet w{};
        w.dstSet = collect_set; w.dstBinding = 2; w.descriptorCount = 1;
        w.descriptorType = vk::DescriptorType::eStorageBuffer; w.pBufferInfo = &bi;
        dev.updateDescriptorSets(w, nullptr);
    }
    {
        const vk::DescriptorBufferInfo bi(*intermediate_buffer.GetBuffer(), 0, intermediate_buffer.GetSize());
        vk::WriteDescriptorSet w{};
        w.dstSet = collect_set; w.dstBinding = 3; w.descriptorCount = 1;
        w.descriptorType = vk::DescriptorType::eStorageBuffer; w.pBufferInfo = &bi;
        dev.updateDescriptorSets(w, nullptr);
    }

    // Write set bindings
    {
        const vk::DescriptorBufferInfo bi(*intermediate_buffer.GetBuffer(), 0, intermediate_buffer.GetSize());
        vk::WriteDescriptorSet w{};
        w.dstSet = collect_write_set; w.dstBinding = 0; w.descriptorCount = 1;
        w.descriptorType = vk::DescriptorType::eStorageBuffer; w.pBufferInfo = &bi;
        dev.updateDescriptorSets(w, nullptr);
    }
    {
        const vk::DescriptorBufferInfo bi(*tech_counts_buffer.GetBuffer(), 0, tech_counts_buffer.GetSize());
        vk::WriteDescriptorSet w{};
        w.dstSet = collect_write_set; w.dstBinding = 1; w.descriptorCount = 1;
        w.descriptorType = vk::DescriptorType::eStorageBuffer; w.pBufferInfo = &bi;
        dev.updateDescriptorSets(w, nullptr);
    }
    {
        const vk::DescriptorBufferInfo bi(*tech_offsets_buffer.GetBuffer(), 0, tech_offsets_buffer.GetSize());
        vk::WriteDescriptorSet w{};
        w.dstSet = collect_write_set; w.dstBinding = 2; w.descriptorCount = 1;
        w.descriptorType = vk::DescriptorType::eStorageBuffer; w.pBufferInfo = &bi;
        dev.updateDescriptorSets(w, nullptr);
    }
    {
        const vk::DescriptorBufferInfo bi(*technique_draw_commands.GetBuffer(), 0, technique_draw_commands.GetSize());
        vk::WriteDescriptorSet w{};
        w.dstSet = collect_write_set; w.dstBinding = 3; w.descriptorCount = 1;
        w.descriptorType = vk::DescriptorType::eStorageBuffer; w.pBufferInfo = &bi;
        dev.updateDescriptorSets(w, nullptr);
    }

    // Pass 0: count
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *count_pipeline_);
    {
        const std::array<vk::DescriptorSet, 1> ds{ collect_set };
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *count_pipeline_layout_, 0, ds, {});
    }
    CollectPC pc0{ entity_count, 0, MAX_TECHNIQUES, 0 };
    cmd.pushConstants(*count_pipeline_layout_, vk::ShaderStageFlagBits::eCompute, 0, sizeof(CollectPC), &pc0);
    cmd.dispatch((entity_count + 255) / 256, 1, 1);

    // Barrier
    {
        vk::MemoryBarrier mb{};
        mb.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
        mb.dstAccessMask = vk::AccessFlagBits::eShaderRead;
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                            vk::PipelineStageFlagBits::eComputeShader, {}, mb, {}, {});
    }

    // Pass 1: compact
    CollectPC pc1{ entity_count, 0, MAX_TECHNIQUES, 1 };
    cmd.pushConstants(*count_pipeline_layout_, vk::ShaderStageFlagBits::eCompute, 0, sizeof(CollectPC), &pc1);
    cmd.dispatch((entity_count + 255) / 256, 1, 1);

    // Barrier
    {
        vk::MemoryBarrier mb{};
        mb.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
        mb.dstAccessMask = vk::AccessFlagBits::eShaderRead;
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                            vk::PipelineStageFlagBits::eComputeShader, {}, mb, {}, {});
    }

    // Pass 2: write
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *write_pipeline_);
    {
        const std::array<vk::DescriptorSet, 1> ds{ collect_write_set };
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *write_pipeline_layout_, 0, ds, {});
    }
    WritePC pc2{ entity_count, 0, MAX_TECHNIQUES, 0 };
    cmd.pushConstants(*write_pipeline_layout_, vk::ShaderStageFlagBits::eCompute, 0, sizeof(WritePC), &pc2);
    cmd.dispatch(1, 1, 1);
}

void CollectPass::Setup(VulkanEngine::PipelinePass::PassSetupContext& /*ctx*/) {}

void CollectPass::Execute(const VulkanEngine::PipelinePass::FrameContext& /*ctx*/,
                           vk::CommandBuffer /*cmd*/) {}

} // namespace VulkanEngine::SceneRenderer
