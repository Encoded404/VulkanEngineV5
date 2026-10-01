module;

#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_core.h>

#include <logging/logging_macros.hpp>

module TestSupport.TextureRenderHarness;

import std;

import logiface;

import vulkan_hpp;

import ShaderReflection;
import VulkanBackend.Vulkan.VulkanBootstrap;
import VulkanBackend.Vulkan.VulkanCapabilities;
import VulkanBackend.Vulkan.MemoryUtils;
import VulkanEngine.ShaderManager;
import VulkanEngine.PipelineFactory;
import VulkanEngine.GpuResources.GpuImageHeap;
import VulkanEngine.GpuTexture;
import TestSupport.HeadlessVulkanBackend;

namespace TestSupport {

namespace {

// One bindless slot: the albedo being sampled.
constexpr std::uint32_t kBindlessSlots = 1;

}  // namespace

HarnessShaderIds RegisterHarnessShaders(VulkanEngine::ShaderSystem::ShaderManager& shaders,
                                        std::string_view shader_dir) {
    HarnessShaderIds ids{};
    const std::string dir(shader_dir);
    ids.vertex = shaders.RegisterManual(std::format("{}/test_fullscreen_vs.spv", dir), "",
                                        ShaderStage::eVertex);
    ids.sample_fragment = shaders.RegisterManual(std::format("{}/test_sample_frag.spv", dir), "",
                                                 ShaderStage::eFragment);
    return ids;
}

TextureRenderHarness::~TextureRenderHarness() {
    Shutdown();
}

void TextureRenderHarness::Shutdown() {
    pipeline_.reset();
    pipeline_layout_.reset();
    bindless_set0_.reset();
    pool_.reset();
    bindless_set0_layout_.reset();
    backend_ = nullptr;
    shaders_ = nullptr;
}

bool TextureRenderHarness::CreateDescriptorLayouts() {
    const vk::raii::Device& device = backend_->GetDevice();
    const vk::DescriptorSetLayoutBinding binding(
        0, vk::DescriptorType::eCombinedImageSampler, kBindlessSlots,
        vk::ShaderStageFlagBits::eFragment, nullptr);
    bindless_set0_layout_ = std::make_unique<vk::raii::DescriptorSetLayout>(
        device, vk::DescriptorSetLayoutCreateInfo{{}, 1, &binding});
    return bindless_set0_layout_ != nullptr;
}

bool TextureRenderHarness::CreatePipeline() {
    const vk::raii::Device& device = backend_->GetDevice();

    const std::array<vk::DescriptorSetLayout, 1> set_layouts{**bindless_set0_layout_};
    vk::PipelineLayoutCreateInfo layout_info{};
    layout_info.setSetLayouts(set_layouts);
    pipeline_layout_ = std::make_unique<vk::raii::PipelineLayout>(device, layout_info);

    const vk::PipelineColorBlendAttachmentState blend(
        vk::False, {}, {}, {}, {}, {}, {},
        vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
            vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA);

    VulkanEngine::ShaderSystem::GraphicsPipelineDesc desc{};
    desc.vertex_shader = config_.vertex_shader;
    desc.fragment_shader = config_.fragment_shader;
    desc.vertex_input = vk::PipelineVertexInputStateCreateInfo({}, 0, nullptr, 0, nullptr);
    desc.input_assembly = vk::PipelineInputAssemblyStateCreateInfo({}, vk::PrimitiveTopology::eTriangleList);
    desc.viewport = vk::PipelineViewportStateCreateInfo({}, 1, nullptr, 1, nullptr);
    desc.rasterization = vk::PipelineRasterizationStateCreateInfo(
        {}, vk::False, vk::False, vk::PolygonMode::eFill, vk::CullModeFlagBits::eNone,
        vk::FrontFace::eCounterClockwise, vk::False, 0.0f, 0.0f, 0.0f, 1.0f);
    desc.multisample = vk::PipelineMultisampleStateCreateInfo({}, vk::SampleCountFlagBits::e1);
    desc.depth_stencil = vk::PipelineDepthStencilStateCreateInfo({}, vk::False, vk::False, vk::CompareOp::eAlways);
    desc.color_blend = vk::PipelineColorBlendStateCreateInfo({}, vk::False, vk::LogicOp::eCopy, blend);
    desc.dynamic_states = {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
    desc.layout = **pipeline_layout_;
    desc.color_formats = {vk::Format::eR8G8B8A8Unorm};

    VulkanEngine::ShaderSystem::PipelineFactory factory(device, backend_->GetCapabilities(),
                                                        shaders_->GetPipelineCacheRAII());
    auto product = factory.CreateGraphics(desc, *shaders_);
    if (!product.has_value()) {
        LOGIFACE_LOG(error, "TextureRenderHarness: pipeline creation failed: " + product.error().message);
        return false;
    }
    // PipelineProduct owns its GPL library pipelines; keep the product itself so
    // a fast-linked pipeline cannot outlive its libraries.
    pipeline_ = std::make_unique<VulkanEngine::ShaderSystem::PipelineProduct>(std::move(*product));
    return true;
}

bool TextureRenderHarness::Initialize(TestSupport::HeadlessVulkanBackend& backend,
                                      VulkanEngine::ShaderSystem::ShaderManager& shaders,
                                      const TextureRenderHarnessConfig& config) {
    config_ = config;
    if (pipeline_ != nullptr) {
        return true;
    }
    backend_ = &backend;
    shaders_ = &shaders;
    if (!backend_->Initialize()) {
        return false;
    }
    if (!CreateDescriptorLayouts()) {
        return false;
    }

    const vk::DescriptorPoolSize size(vk::DescriptorType::eCombinedImageSampler, kBindlessSlots);
    pool_ = std::make_unique<vk::raii::DescriptorPool>(
        backend_->GetDevice(), vk::DescriptorPoolCreateInfo({}, 1, 1, &size));

    const std::array<vk::DescriptorSetLayout, 1> layouts{**bindless_set0_layout_};
    auto sets = backend_->GetDevice().allocateDescriptorSets(
        vk::DescriptorSetAllocateInfo{**pool_, 1, layouts.data()});
    if (sets.size() != 1U) {
        return false;
    }
    bindless_set0_ = std::make_unique<vk::raii::DescriptorSet>(std::move(sets[0]));
    return CreatePipeline();
}

void TextureRenderHarness::WriteDescriptors(const VulkanEngine::GpuResources::GpuTexture& albedo) {
    const vk::DescriptorImageInfo image_info(albedo.GetSampler(), albedo.GetImageView(),
                                             vk::ImageLayout::eShaderReadOnlyOptimal);
    const vk::WriteDescriptorSet write(*bindless_set0_, 0, 0, kBindlessSlots,
                                       vk::DescriptorType::eCombinedImageSampler, &image_info);
    backend_->GetDevice().updateDescriptorSets(write, {});
}

std::vector<std::uint8_t> TextureRenderHarness::RenderAndReadBack(
    const VulkanEngine::GpuResources::GpuTexture& albedo) {
    if (!albedo.IsValid()) {
        return {};
    }
    const vk::raii::Device& device = backend_->GetDevice();
    const std::uint32_t width = config_.width;
    const std::uint32_t height = config_.height;
    const vk::DeviceSize pixel_bytes = static_cast<vk::DeviceSize>(width) * height * 4U;

    const vk::ImageCreateInfo color_info(
        {}, vk::ImageType::e2D, vk::Format::eR8G8B8A8Unorm, vk::Extent3D{width, height, 1},
        1, 1, vk::SampleCountFlagBits::e1, vk::ImageTiling::eOptimal,
        vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc,
        vk::SharingMode::eExclusive);
    vk::raii::Image color_image(device, color_info);
    const vk::MemoryRequirements color_req = color_image.getMemoryRequirements();
    vk::raii::DeviceMemory color_memory(
        device, vk::MemoryAllocateInfo(
                    color_req.size,
                    VulkanBackend::Vulkan::MemoryUtils::FindMemoryType(
                        backend_->GetPhysicalDevice(), color_req.memoryTypeBits,
                        vk::MemoryPropertyFlagBits::eDeviceLocal)));
    color_image.bindMemory(*color_memory, 0);
    const vk::raii::ImageView color_view(
        device, vk::ImageViewCreateInfo({}, *color_image, vk::ImageViewType::e2D,
                                        vk::Format::eR8G8B8A8Unorm, vk::ComponentMapping{},
                                        vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)));

    const vk::raii::Buffer readback_buffer(
        device, vk::BufferCreateInfo({}, pixel_bytes, vk::BufferUsageFlagBits::eTransferDst));
    const vk::MemoryRequirements read_req = readback_buffer.getMemoryRequirements();
    vk::raii::DeviceMemory read_memory(
        device, vk::MemoryAllocateInfo(
                    read_req.size,
                    VulkanBackend::Vulkan::MemoryUtils::FindMemoryType(
                        backend_->GetPhysicalDevice(), read_req.memoryTypeBits,
                        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent)));
    readback_buffer.bindMemory(*read_memory, 0);

    WriteDescriptors(albedo);

    vk::raii::CommandBuffer& cmd = backend_->GetCommandBuffer(0);
    cmd.reset({});
    cmd.begin({vk::CommandBufferUsageFlagBits::eOneTimeSubmit});

    const vk::ImageSubresourceRange range(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
    const vk::ImageMemoryBarrier to_color(
        {}, vk::AccessFlagBits::eColorAttachmentWrite, vk::ImageLayout::eUndefined,
        vk::ImageLayout::eColorAttachmentOptimal, vk::QueueFamilyIgnored, vk::QueueFamilyIgnored,
        *color_image, range);
    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe, vk::PipelineStageFlagBits::eColorAttachmentOutput,
                        {}, {}, {}, to_color);

    const vk::ClearValue clear(vk::ClearColorValue(std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f}));
    vk::RenderingAttachmentInfo color_attachment{};
    color_attachment.imageView = *color_view;
    color_attachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
    color_attachment.resolveMode = vk::ResolveModeFlagBits::eNone;
    color_attachment.loadOp = vk::AttachmentLoadOp::eClear;
    color_attachment.storeOp = vk::AttachmentStoreOp::eStore;
    color_attachment.clearValue = clear;
    vk::RenderingInfo rendering{};
    rendering.renderArea = vk::Rect2D({0, 0}, {width, height});
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color_attachment;

    cmd.beginRendering(rendering);
    cmd.setViewport(0, vk::Viewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f));
    cmd.setScissor(0, vk::Rect2D({0, 0}, {width, height}));
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline_->Get());
    const std::array<vk::DescriptorSet, 1> sets{**bindless_set0_};
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, **pipeline_layout_, 0, sets, {});
    cmd.draw(3, 1, 0, 0);
    cmd.endRendering();

    const vk::ImageMemoryBarrier to_src(
        vk::AccessFlagBits::eColorAttachmentWrite, vk::AccessFlagBits::eTransferRead,
        vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::eTransferSrcOptimal,
        vk::QueueFamilyIgnored, vk::QueueFamilyIgnored, *color_image, range);
    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput, vk::PipelineStageFlagBits::eTransfer,
                        {}, {}, {}, to_src);

    const vk::BufferImageCopy region(0, 0, 0, vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1),
                                     vk::Offset3D{0, 0, 0}, vk::Extent3D{width, height, 1});
    cmd.copyImageToBuffer(*color_image, vk::ImageLayout::eTransferSrcOptimal, *readback_buffer, region);

    const vk::BufferMemoryBarrier to_host(
        vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eHostRead, vk::QueueFamilyIgnored,
        vk::QueueFamilyIgnored, *readback_buffer, 0, pixel_bytes);
    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost,
                        {}, {}, to_host, {});
    cmd.end();

    const vk::SubmitInfo submit(0, nullptr, nullptr, 1, &*cmd);
    backend_->GetGraphicsQueue().submit(submit, {});
    backend_->GetGraphicsQueue().waitIdle();

    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(pixel_bytes));
    const void* mapped = read_memory.mapMemory(0, pixel_bytes);
    std::memcpy(pixels.data(), mapped, pixels.size());
    read_memory.unmapMemory();
    return pixels;
}

}  // namespace TestSupport
