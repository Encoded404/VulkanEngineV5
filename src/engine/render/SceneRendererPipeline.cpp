module;

#include <glm/glm.hpp> // NOLINT(misc-include-cleaner)

#include <logging/logging_macros.hpp>

module VulkanEngine.SceneRenderer;

import std;
import std.compat;

import logiface;

import vulkan_hpp;

import VulkanBackend.Vulkan.VulkanDebugUtils;

namespace VulkanEngine::SceneRenderer {
    namespace {
        struct ExpandPC { glm::mat4 vp; std::uint32_t cnt; std::uint32_t p0; std::uint32_t p1; };
        // projInfo = (|proj[0][0]|, |proj[1][1]|, proj[2][2], 1 if perspective else 0)
        struct OccPC { std::uint32_t cnt; std::uint32_t refineLevel; std::uint32_t hizWidth; std::uint32_t hizHeight; glm::vec4 projInfo; };
        struct HiZPC { std::uint32_t bl; std::uint32_t sw; std::uint32_t sh; std::uint32_t tc; };
        struct CollectPC { std::uint32_t cnt; std::uint32_t p0; std::uint32_t mt; std::uint32_t pass; };
        struct WritePC { std::uint32_t cnt; std::uint32_t p0; std::uint32_t techniqueCount; std::uint32_t p1; };

    } // anonymous namespace

bool SceneRenderer::CreateExpandPipeline(const VulkanBackend::Vulkan::IVulkanBootstrap& be,
                                          ShaderSystem::ShaderManager& shader_mgr,
                                          ShaderSystem::PipelineFactory& pipeline_factory,
                                          ShaderSystem::ShaderId shader_id) {
    LOGIFACE_LOG(debug, "Creating expand pipeline...");
    const auto& dev = be.GetDevice();
    vk::PushConstantRange pr{};
    pr.stageFlags = vk::ShaderStageFlagBits::eCompute;
    pr.size = sizeof(ExpandPC);
    std::array<vk::DescriptorSetLayout, 2> sl{ *expand_layout_, *bindless_index_layout_ };
    vk::PipelineLayoutCreateInfo li{};
    li.setLayoutCount = static_cast<std::uint32_t>(sl.size());
    li.pSetLayouts = sl.data();
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &pr;
    expand_pipeline_layout_ = std::make_unique<vk::raii::PipelineLayout>(dev, li);
    VulkanBackend::Vulkan::SetVulkanObjectName(dev, *expand_pipeline_layout_, "expand-pipeline-layout");

    ShaderSystem::ComputePipelineDesc desc{};
    desc.shader = shader_id;
    desc.layout = *expand_pipeline_layout_;
    expand_desc_ = desc;
    auto result = pipeline_factory.CreateCompute(desc, shader_mgr);
    if (result.has_value()) {
        expand_slot_.Swap(std::move(result.value()), 0);
        VulkanBackend::Vulkan::SetVulkanObjectName(dev, expand_slot_.Get(), "expand-pipeline");
    }
    LOGIFACE_LOG(debug, "Expand pipeline created");
    return true;
}

bool SceneRenderer::CreateDepthPipeline(VulkanBackend::Vulkan::IVulkanBootstrap& be,
                                         ShaderSystem::ShaderManager& shader_mgr,
                                         ShaderSystem::PipelineFactory& pipeline_factory,
                                         ShaderSystem::ShaderId vert_id,
                                         ShaderSystem::ShaderId frag_id,
                                         const vk::PipelineRasterizationStateCreateInfo& rs) {
    LOGIFACE_LOG(debug, "Creating depth pipeline...");
    const auto& dev = be.GetDevice();
    std::array<vk::DescriptorSetLayout, 4> sl{
        *empty_layout_, *submesh_vertex_layout_, *raw_vertex_layout_, *indirection_layout_
    };
    vk::PipelineLayoutCreateInfo li{};
    li.setLayoutCount = static_cast<std::uint32_t>(sl.size());
    li.pSetLayouts = sl.data();
    depth_pipeline_layout_ = std::make_unique<vk::raii::PipelineLayout>(dev, li);
    VulkanBackend::Vulkan::SetVulkanObjectName(dev, *depth_pipeline_layout_, "depth-prepass-pipeline-layout");

    ShaderSystem::GraphicsPipelineDesc desc{};
    desc.vertex_shader = vert_id;
    desc.fragment_shader = frag_id;
    desc.vertex_input = vk::PipelineVertexInputStateCreateInfo({}, 0, nullptr, 0, nullptr);
    desc.input_assembly = vk::PipelineInputAssemblyStateCreateInfo({}, vk::PrimitiveTopology::eTriangleList);
    desc.viewport = vk::PipelineViewportStateCreateInfo({}, 1, nullptr, 1, nullptr);
    desc.rasterization = rs;
    desc.multisample = vk::PipelineMultisampleStateCreateInfo({}, vk::SampleCountFlagBits::e1);
    desc.depth_stencil = vk::PipelineDepthStencilStateCreateInfo({}, true, true, vk::CompareOp::eLess);
    // Depth-only pass: no color attachments, so the blend state must not declare any.
    desc.color_blend = vk::PipelineColorBlendStateCreateInfo({}, false, vk::LogicOp::eCopy, 0, nullptr);
    desc.dynamic_states = { vk::DynamicState::eViewport, vk::DynamicState::eScissor };
    desc.layout = *depth_pipeline_layout_;
    desc.depth_format = be.GetDepthFormat();
    depth_desc_ = desc;

    auto result = pipeline_factory.CreateGraphics(desc, shader_mgr);
    if (result.has_value()) {
        depth_slot_.Swap(std::move(result.value()), 0);
        VulkanBackend::Vulkan::SetVulkanObjectName(dev, depth_slot_.Get(), "depth-prepass-pipeline");
    }
    LOGIFACE_LOG(debug, "Depth pipeline created");
    return true;
}

bool SceneRenderer::CreateHiZPipeline(VulkanBackend::Vulkan::IVulkanBootstrap& be,
                                       ShaderSystem::ShaderManager& shader_mgr,
                                       ShaderSystem::PipelineFactory& pipeline_factory,
                                       ShaderSystem::ShaderId shader_id) {
    LOGIFACE_LOG(debug, "Creating HIZ pipeline...");
    const auto& dev = be.GetDevice();
    vk::PushConstantRange pr{};
    pr.stageFlags = vk::ShaderStageFlagBits::eCompute;
    pr.size = sizeof(HiZPC);
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
    hiz_layout_ = std::make_unique<vk::raii::DescriptorSetLayout>(
        dev, vk::DescriptorSetLayoutCreateInfo{{}, static_cast<std::uint32_t>(bs.size()), bs.data()});
    GpuResources::DescriptorPoolConfig pc{};
    pc.max_sets = frames_in_flight_;
    pc.max_storage_images = frames_in_flight_ * MAX_HIZ_MIPS;
    pc.max_sampled_images = frames_in_flight_;
    pc.max_samplers = frames_in_flight_;
    hiz_pool_ = GpuResources::DescriptorPool::Create(be, pc);
    vk::PipelineLayoutCreateInfo li{};
    li.setLayoutCount = 1;
    li.pSetLayouts = &**hiz_layout_;
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &pr;
    hiz_pipeline_layout_ = std::make_unique<vk::raii::PipelineLayout>(dev, li);
    VulkanBackend::Vulkan::SetVulkanObjectName(dev, *hiz_pipeline_layout_, "hiz-gen-pipeline-layout");

    ShaderSystem::ComputePipelineDesc desc{};
    desc.shader = shader_id;
    desc.layout = *hiz_pipeline_layout_;
    hiz_desc_ = desc;
    auto result = pipeline_factory.CreateCompute(desc, shader_mgr);
    if (result.has_value()) {
        hiz_slot_.Swap(std::move(result.value()), 0);
        VulkanBackend::Vulkan::SetVulkanObjectName(dev, hiz_slot_.Get(), "hiz-gen-pipeline");
    }
    LOGIFACE_LOG(debug, "HiZ pipeline created");
    return true;
}

bool SceneRenderer::CreateOcclusionPipeline(const VulkanBackend::Vulkan::IVulkanBootstrap& be,
                                              ShaderSystem::ShaderManager& shader_mgr,
                                              ShaderSystem::PipelineFactory& pipeline_factory,
                                              ShaderSystem::ShaderId shader_id) {
    LOGIFACE_LOG(debug, "Creating occlusion pipeline...");
    const auto& dev = be.GetDevice();
    vk::PushConstantRange pr{};
    pr.stageFlags = vk::ShaderStageFlagBits::eCompute;
    pr.size = sizeof(OccPC);
    vk::PipelineLayoutCreateInfo li{};
    li.setLayoutCount = 1;
    li.pSetLayouts = &**occlusion_layout_;
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &pr;
    occlusion_pipeline_layout_ = std::make_unique<vk::raii::PipelineLayout>(dev, li);
    VulkanBackend::Vulkan::SetVulkanObjectName(dev, *occlusion_pipeline_layout_, "occlusion-pipeline-layout");

    ShaderSystem::ComputePipelineDesc desc{};
    desc.shader = shader_id;
    desc.layout = *occlusion_pipeline_layout_;
    occlusion_desc_ = desc;
    auto result = pipeline_factory.CreateCompute(desc, shader_mgr);
    if (result.has_value()) {
        occlusion_slot_.Swap(std::move(result.value()), 0);
        VulkanBackend::Vulkan::SetVulkanObjectName(dev, occlusion_slot_.Get(), "occlusion-pipeline");
    }
    LOGIFACE_LOG(debug, "Occlusion pipeline created");
    return true;
}

bool SceneRenderer::CreateCollectPipelines(const VulkanBackend::Vulkan::IVulkanBootstrap& be,
                                             ShaderSystem::ShaderManager& shader_mgr,
                                             ShaderSystem::PipelineFactory& pipeline_factory,
                                             ShaderSystem::ShaderId count_id,
                                             ShaderSystem::ShaderId write_id) {
    LOGIFACE_LOG(debug, "Creating collect pipelines...");
    const auto& dev = be.GetDevice();

    {
        vk::PushConstantRange pr{};
        pr.stageFlags = vk::ShaderStageFlagBits::eCompute;
        pr.size = sizeof(CollectPC);
        vk::PipelineLayoutCreateInfo li{};
        li.setLayoutCount = 1;
        li.pSetLayouts = &**collect_layout_;
        li.pushConstantRangeCount = 1;
        li.pPushConstantRanges = &pr;
        collect_pipeline_layout_ = std::make_unique<vk::raii::PipelineLayout>(dev, li);
        VulkanBackend::Vulkan::SetVulkanObjectName(dev, *collect_pipeline_layout_, "collect-count-pipeline-layout");

        ShaderSystem::ComputePipelineDesc desc{};
        desc.shader = count_id;
        desc.layout = *collect_pipeline_layout_;
        collect_count_desc_ = desc;
        auto result = pipeline_factory.CreateCompute(desc, shader_mgr);
        if (result.has_value()) {
            collect_count_slot_.Swap(std::move(result.value()), 0);
            VulkanBackend::Vulkan::SetVulkanObjectName(dev, collect_count_slot_.Get(), "collect-count-pipeline");
        }
    }

    {
        vk::PushConstantRange pr{};
        pr.stageFlags = vk::ShaderStageFlagBits::eCompute;
        pr.size = sizeof(WritePC);
        vk::PipelineLayoutCreateInfo li{};
        li.setLayoutCount = 1;
        li.pSetLayouts = &**collect_write_layout_;
        li.pushConstantRangeCount = 1;
        li.pPushConstantRanges = &pr;
        collect_write_pipeline_layout_ = std::make_unique<vk::raii::PipelineLayout>(dev, li);
        VulkanBackend::Vulkan::SetVulkanObjectName(dev, *collect_write_pipeline_layout_, "collect-write-pipeline-layout");

        ShaderSystem::ComputePipelineDesc desc{};
        desc.shader = write_id;
        desc.layout = *collect_write_pipeline_layout_;
        collect_write_desc_ = desc;
        auto result = pipeline_factory.CreateCompute(desc, shader_mgr);
        if (result.has_value()) {
            collect_write_slot_.Swap(std::move(result.value()), 0);
            VulkanBackend::Vulkan::SetVulkanObjectName(dev, collect_write_slot_.Get(), "collect-write-pipeline");
        }
    }

    LOGIFACE_LOG(debug, "Collect pipelines created");
    return true;
}

} // namespace VulkanEngine::SceneRenderer
