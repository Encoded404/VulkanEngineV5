module;

#include <cstdint>
#include <array>
#include <filesystem>
#include <string>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_RADIANS
#include <glm/glm.hpp> //NOLINT(misc-include-cleaner)

#include <vulkan/vulkan_raii.hpp>
#include <logging/logging.hpp>

module VulkanEngine.SceneRenderer;

import VulkanEngine.ShaderLoader;
import VulkanBackend.Utils.VulkanDebugUtils;

namespace VulkanEngine::SceneRenderer {
    namespace {
        struct ExpandPC { glm::mat4 vp; uint32_t cnt; uint32_t p0; uint32_t p1; };
        struct OccPC { uint32_t cnt; uint32_t refineLevel; uint32_t hizWidth; uint32_t hizHeight; };
        struct HiZPC { uint32_t bl; uint32_t sw; uint32_t sh; uint32_t tc; };
        struct CollectPC { uint32_t cnt; uint32_t p0; uint32_t mt; uint32_t pass; };

    } // anonymous namespace

bool SceneRenderer::CreateExpandPipeline(const VulkanEngine::Runtime::IVulkanBootstrapBackend& be) {
    LOGIFACE_LOG(info, "Creating expand pipeline...");
    const auto& dev = be.GetDevice();
    vk::PushConstantRange pr{};
    pr.stageFlags = vk::ShaderStageFlagBits::eCompute;
    pr.size = sizeof(ExpandPC);
    std::array<vk::DescriptorSetLayout, 2> sl{ *expand_layout_, *bindless_index_layout_ };
    vk::PipelineLayoutCreateInfo li{};
    li.setLayoutCount = static_cast<uint32_t>(sl.size());
    li.pSetLayouts = sl.data();
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &pr;
    expand_pipeline_layout_ = std::make_unique<vk::raii::PipelineLayout>(dev, li);
    VulkanEngine::Utils::SetVulkanObjectName(dev, *expand_pipeline_layout_, "expand-pipeline-layout");
    auto spv = VulkanEngine::ShaderLoader::ShaderLoader::LoadSpirv(
        std::filesystem::path(SHADER_DIR) / "expand.comp.spv");
    if (spv.empty()) {
        LOGIFACE_LOG(error, "Failed to load expand.comp");
        return false;
    }
    const vk::ShaderModuleCreateInfo mi({}, spv.size() * sizeof(uint32_t), spv.data());
    const vk::raii::ShaderModule mod(dev, mi);
    const vk::PipelineShaderStageCreateInfo ss({}, vk::ShaderStageFlagBits::eCompute, *mod, "main");
    vk::ComputePipelineCreateInfo ci{};
    ci.stage = ss;
    ci.layout = *expand_pipeline_layout_;
    expand_pipeline_ = std::make_unique<vk::raii::Pipeline>(dev, nullptr, ci);
    VulkanEngine::Utils::SetVulkanObjectName(dev, *expand_pipeline_, "expand-pipeline");
    LOGIFACE_LOG(info, "Expand pipeline created");
    return true;
}

bool SceneRenderer::CreateDepthPipeline(VulkanEngine::Runtime::IVulkanBootstrapBackend& be,
                                         const vk::PipelineRasterizationStateCreateInfo& rs) {
    LOGIFACE_LOG(info, "Creating depth pipeline...");
    const auto& dev = be.GetDevice();
    std::array<vk::DescriptorSetLayout, 4> sl{
        *empty_layout_, *submesh_vertex_layout_, *raw_vertex_layout_, *indirection_layout_
    };
    vk::PipelineLayoutCreateInfo li{};
    li.setLayoutCount = static_cast<uint32_t>(sl.size());
    li.pSetLayouts = sl.data();
    depth_pipeline_layout_ = std::make_unique<vk::raii::PipelineLayout>(dev, li);
    VulkanEngine::Utils::SetVulkanObjectName(dev, *depth_pipeline_layout_, "depth-prepass-pipeline-layout");
    auto vspv = VulkanEngine::ShaderLoader::ShaderLoader::LoadSpirv(
        std::filesystem::path(SHADER_DIR) / "depth_world.vert.spv");
    auto fspv = VulkanEngine::ShaderLoader::ShaderLoader::LoadSpirv(
        std::filesystem::path(SHADER_DIR) / "depth_prepass.frag.spv");
    if (vspv.empty() || fspv.empty()) {
        LOGIFACE_LOG(error, "Failed to load depth shaders");
        return false;
    }
    const vk::ShaderModuleCreateInfo vmi({}, vspv.size() * sizeof(uint32_t), vspv.data());
    const vk::ShaderModuleCreateInfo fmi({}, fspv.size() * sizeof(uint32_t), fspv.data());
    const vk::raii::ShaderModule vm(dev, vmi), fm(dev, fmi);
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
                                       *depth_pipeline_layout_, nullptr, 0, {}, 0);
    pi.setPNext(&ri);
    depth_pipeline_ = std::make_unique<vk::raii::Pipeline>(dev, nullptr, pi);
    VulkanEngine::Utils::SetVulkanObjectName(dev, *depth_pipeline_, "depth-prepass-pipeline");
    LOGIFACE_LOG(info, "Depth pipeline created");
    return true;
}

bool SceneRenderer::CreateHiZPipeline(VulkanEngine::Runtime::IVulkanBootstrapBackend& be) {
    LOGIFACE_LOG(info, "Creating HIZ pipeline...");
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
        dev, vk::DescriptorSetLayoutCreateInfo{{}, static_cast<uint32_t>(bs.size()), bs.data()});
    GpuResources::DescriptorPoolConfig pc{};
    pc.max_sets = FRAMES_IN_FLIGHT;
    pc.max_storage_images = FRAMES_IN_FLIGHT * MAX_HIZ_MIPS;
    pc.max_sampled_images = FRAMES_IN_FLIGHT;
    pc.max_samplers = FRAMES_IN_FLIGHT;
    hiz_pool_ = GpuResources::DescriptorPool::Create(be, pc);
    vk::PipelineLayoutCreateInfo li{};
    li.setLayoutCount = 1;
    li.pSetLayouts = &**hiz_layout_;
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &pr;
    hiz_pipeline_layout_ = std::make_unique<vk::raii::PipelineLayout>(dev, li);
    VulkanEngine::Utils::SetVulkanObjectName(dev, *hiz_pipeline_layout_, "hiz-gen-pipeline-layout");
    auto spv = VulkanEngine::ShaderLoader::ShaderLoader::LoadSpirv(
        std::filesystem::path(SHADER_DIR) / "hiz_gen.comp.spv");
    if (spv.empty()) {
        LOGIFACE_LOG(error, "Failed to load hiz_gen");
        return false;
    }
    const vk::ShaderModuleCreateInfo mi({}, spv.size() * sizeof(uint32_t), spv.data());
    const vk::raii::ShaderModule mod(dev, mi);
    const vk::PipelineShaderStageCreateInfo ss({}, vk::ShaderStageFlagBits::eCompute, *mod, "main");
    vk::ComputePipelineCreateInfo ci{};
    ci.stage = ss;
    ci.layout = *hiz_pipeline_layout_;
    hiz_pipeline_ = std::make_unique<vk::raii::Pipeline>(dev, nullptr, ci);
    VulkanEngine::Utils::SetVulkanObjectName(dev, *hiz_pipeline_, "hiz-gen-pipeline");
    LOGIFACE_LOG(info, "HiZ pipeline created");
    return true;
}

bool SceneRenderer::CreateOcclusionPipeline(const VulkanEngine::Runtime::IVulkanBootstrapBackend& be) {
    LOGIFACE_LOG(info, "Creating occlusion pipeline...");
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
    VulkanEngine::Utils::SetVulkanObjectName(dev, *occlusion_pipeline_layout_, "occlusion-pipeline-layout");
    auto spv = VulkanEngine::ShaderLoader::ShaderLoader::LoadSpirv(
        std::filesystem::path(SHADER_DIR) / "occlusion_sort.comp.spv");
    if (spv.empty()) {
        LOGIFACE_LOG(error, "Failed to load occlusion_sort");
        return false;
    }
    const vk::ShaderModuleCreateInfo mi({}, spv.size() * sizeof(uint32_t), spv.data());
    const vk::raii::ShaderModule mod(dev, mi);
    const vk::PipelineShaderStageCreateInfo ss({}, vk::ShaderStageFlagBits::eCompute, *mod, "main");
    vk::ComputePipelineCreateInfo ci{};
    ci.stage = ss;
    ci.layout = *occlusion_pipeline_layout_;
    occlusion_pipeline_ = std::make_unique<vk::raii::Pipeline>(dev, nullptr, ci);
    VulkanEngine::Utils::SetVulkanObjectName(dev, *occlusion_pipeline_, "occlusion-pipeline");
    LOGIFACE_LOG(info, "Occlusion pipeline created");
    return true;
}

bool SceneRenderer::CreateCollectPipeline(const VulkanEngine::Runtime::IVulkanBootstrapBackend& be) {
    LOGIFACE_LOG(info, "Creating collect pipeline...");
    const auto& dev = be.GetDevice();
    vk::PushConstantRange pr{};
    pr.stageFlags = vk::ShaderStageFlagBits::eCompute;
    pr.size = sizeof(CollectPC);
    vk::PipelineLayoutCreateInfo li{};
    li.setLayoutCount = 1;
    li.pSetLayouts = &**collect_layout_;
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &pr;
    collect_pipeline_layout_ = std::make_unique<vk::raii::PipelineLayout>(dev, li);
    VulkanEngine::Utils::SetVulkanObjectName(dev, *collect_pipeline_layout_, "collect-pipeline-layout");
    auto spv = VulkanEngine::ShaderLoader::ShaderLoader::LoadSpirv(
        std::filesystem::path(SHADER_DIR) / "collect.comp.spv");
    if (spv.empty()) {
        LOGIFACE_LOG(error, "Failed to load collect.comp");
        return false;
    }
    const vk::ShaderModuleCreateInfo mi({}, spv.size() * sizeof(uint32_t), spv.data());
    const vk::raii::ShaderModule mod(dev, mi);
    const vk::PipelineShaderStageCreateInfo ss({}, vk::ShaderStageFlagBits::eCompute, *mod, "main");
    vk::ComputePipelineCreateInfo ci{};
    ci.stage = ss;
    ci.layout = *collect_pipeline_layout_;
    collect_pipeline_ = std::make_unique<vk::raii::Pipeline>(dev, nullptr, ci);
    VulkanEngine::Utils::SetVulkanObjectName(dev, *collect_pipeline_, "collect-pipeline");
    LOGIFACE_LOG(info, "Collect pipeline created");
    return true;
}

} // namespace VulkanEngine::SceneRenderer
