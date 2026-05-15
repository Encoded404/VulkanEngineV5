module;

#include <cstdint>
#include <memory>
#include <vector>
#include <array>
#include <stdexcept>

#include <vulkan/vulkan_raii.hpp>

#include <logging/logging.hpp>

module VulkanEngine.StandardMeshPipeline;

import VulkanEngine.Runtime.VulkanBootstrap;
import VulkanEngine.GpuResources;

namespace VulkanEngine::StandardMeshPipeline {

PipelineManager::PipelineManager() = default;
PipelineManager::~PipelineManager() = default;

void PipelineManager::Initialize(VulkanEngine::Runtime::VulkanBootstrap& bootstrap,
                                  const std::vector<uint32_t>& vertex_spirv,
                                  const std::vector<uint32_t>& fragment_spirv) {
    bootstrap_ = &bootstrap;
    CreateDescriptorSetLayout(bootstrap);
    CreateDescriptorPool(bootstrap);
    CreatePipeline(bootstrap, vertex_spirv, fragment_spirv);
}

void PipelineManager::Shutdown() {
    if (bootstrap_) {
        bootstrap_->GetBackend().GetDevice().waitIdle();
    }
    pipeline_.reset();
    pipeline_layout_.reset();
    pool_.reset();
    descriptor_set_layout_.reset();
    bootstrap_ = nullptr;
}

vk::PipelineLayout* PipelineManager::GetPipelineLayout() {
    return pipeline_layout_ ? const_cast<vk::PipelineLayout*>(&**pipeline_layout_) : nullptr;
}

const vk::PipelineLayout* PipelineManager::GetPipelineLayout() const {
    return pipeline_layout_ ? &**pipeline_layout_ : nullptr;
}

vk::raii::Pipeline* PipelineManager::GetPipeline() {
    return pipeline_.get();
}

const vk::raii::Pipeline* PipelineManager::GetPipeline() const {
    return pipeline_.get();
}

vk::DescriptorSetLayout* PipelineManager::GetDescriptorSetLayout() {
    return descriptor_set_layout_ ? const_cast<vk::DescriptorSetLayout*>(&**descriptor_set_layout_) : nullptr;
}

const vk::DescriptorSetLayout* PipelineManager::GetDescriptorSetLayout() const {
    return descriptor_set_layout_ ? &**descriptor_set_layout_ : nullptr;
}

VulkanEngine::GpuResources::GpuDescriptorSet PipelineManager::AllocateDescriptorSet() {
    if (!pool_) {
        throw std::runtime_error("Descriptor pool not initialized");
    }
    return pool_->Allocate(*descriptor_set_layout_);
}

VulkanEngine::GpuResources::GpuDescriptorSet PipelineManager::AllocateDescriptorSet(const VulkanEngine::GpuResources::GpuTexture& texture) {
    auto set = AllocateDescriptorSet();
    if (texture.IsValid()) {
        set.UpdateBinding(0, texture);
    }
    return set;
}

void PipelineManager::CreateDescriptorSetLayout(VulkanEngine::Runtime::VulkanBootstrap& bootstrap) {
    LOGIFACE_LOG(trace, "entering CreateDescriptorSetLayout");

    const auto& device = bootstrap.GetBackend().GetDevice();

    std::vector<vk::DescriptorSetLayoutBinding> bindings = {
        {0, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment}
    };

    const vk::DescriptorSetLayoutCreateInfo info({}, bindings);
    descriptor_set_layout_ = std::make_unique<vk::raii::DescriptorSetLayout>(device, info);

    LOGIFACE_LOG(trace, "leaving CreateDescriptorSetLayout successfully");
}

void PipelineManager::CreateDescriptorPool(VulkanEngine::Runtime::VulkanBootstrap& bootstrap) {
    LOGIFACE_LOG(trace, "entering CreateDescriptorPool");

    VulkanEngine::GpuResources::DescriptorPoolConfig config{};
    config.max_sets = 100;
    config.max_combined_image_samplers = 100;
    pool_ = VulkanEngine::GpuResources::DescriptorPool::Create(bootstrap.GetBackend(), config);

    LOGIFACE_LOG(trace, "leaving CreateDescriptorPool successfully");
}

void PipelineManager::CreatePipeline(VulkanEngine::Runtime::VulkanBootstrap& bootstrap,
                                      const std::vector<uint32_t>& vert_spv,
                                      const std::vector<uint32_t>& frag_spv) {
    LOGIFACE_LOG(trace, "entering CreatePipeline");

    const auto& device = bootstrap.GetBackend().GetDevice();

    const vk::ShaderModuleCreateInfo vert_info({}, vert_spv.size() * sizeof(uint32_t), vert_spv.data());
    const vk::raii::ShaderModule vert_module(device, vert_info);

    const vk::ShaderModuleCreateInfo frag_info({}, frag_spv.size() * sizeof(uint32_t), frag_spv.data());
    const vk::raii::ShaderModule frag_module(device, frag_info);

    std::array<vk::PipelineShaderStageCreateInfo, 2> stages = {
        vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eVertex, *vert_module, "main"),
        vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eFragment, *frag_module, "main")
    };

    const std::array<vk::DescriptorSetLayout, 1> set_layouts = { **descriptor_set_layout_ };

    constexpr uint32_t push_constant_size = 64;
    constexpr vk::PushConstantRange push_range(vk::ShaderStageFlagBits::eVertex, 0, push_constant_size);

    vk::PipelineLayoutCreateInfo layout_info{};
    layout_info.setLayoutCount = static_cast<uint32_t>(set_layouts.size());
    layout_info.pSetLayouts = set_layouts.data();
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_range;
    pipeline_layout_ = std::make_unique<vk::raii::PipelineLayout>(device, layout_info);

    std::array<vk::VertexInputBindingDescription, 1> vertex_bindings = {
        vk::VertexInputBindingDescription(0, sizeof(Vertex), vk::VertexInputRate::eVertex)
    };

    std::array<vk::VertexInputAttributeDescription, 3> vertex_attributes = {
        vk::VertexInputAttributeDescription{0, 0, vk::Format::eR32G32B32Sfloat, offsetof(Vertex, px)},
        vk::VertexInputAttributeDescription{1, 0, vk::Format::eR32G32B32Sfloat, offsetof(Vertex, nx)},
        vk::VertexInputAttributeDescription{2, 0, vk::Format::eR32G32Sfloat, offsetof(Vertex, u)}
    };

    const vk::PipelineVertexInputStateCreateInfo vertex_input({}, vertex_bindings, vertex_attributes);
    constexpr vk::PipelineInputAssemblyStateCreateInfo input_assembly({}, vk::PrimitiveTopology::eTriangleList);
    constexpr vk::PipelineViewportStateCreateInfo viewport_state({}, 1, nullptr, 1, nullptr);
    constexpr vk::PipelineRasterizationStateCreateInfo rasterization({}, false, false, vk::PolygonMode::eFill, vk::CullModeFlagBits::eBack, vk::FrontFace::eCounterClockwise, false, 0, 0, 0, 1.0f);
    constexpr vk::PipelineMultisampleStateCreateInfo multisample({}, vk::SampleCountFlagBits::e1);
    constexpr vk::PipelineDepthStencilStateCreateInfo depth_stencil({}, true, true, vk::CompareOp::eLess);

    constexpr vk::PipelineColorBlendAttachmentState color_blend_attachment(false, vk::BlendFactor::eZero, vk::BlendFactor::eZero, vk::BlendOp::eAdd, vk::BlendFactor::eZero, vk::BlendFactor::eZero, vk::BlendOp::eAdd, vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA);
    const vk::PipelineColorBlendStateCreateInfo color_blend({}, false, vk::LogicOp::eCopy, color_blend_attachment);

    std::array<vk::DynamicState, 2> dynamic_states = {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
    const vk::PipelineDynamicStateCreateInfo dynamic_state({}, dynamic_states);

    const vk::Format format = bootstrap.GetBackend().GetSurfaceFormat().format;
    vk::PipelineRenderingCreateInfo rendering_info{};
    rendering_info.colorAttachmentCount = 1;
    rendering_info.pColorAttachmentFormats = &format;
    rendering_info.depthAttachmentFormat = bootstrap.GetBackend().GetDepthFormat();
    rendering_info.stencilAttachmentFormat = vk::Format::eUndefined;

    vk::GraphicsPipelineCreateInfo pipeline_info({}, stages, &vertex_input, &input_assembly, nullptr, &viewport_state, &rasterization, &multisample, &depth_stencil, &color_blend, &dynamic_state, *pipeline_layout_, nullptr, 0, {}, 0);
    pipeline_info.setPNext(&rendering_info);

    pipeline_ = std::make_unique<vk::raii::Pipeline>(device, nullptr, pipeline_info);

    LOGIFACE_LOG(trace, "leaving CreatePipeline successfully");
}

} // namespace VulkanEngine::StandardMeshPipeline
