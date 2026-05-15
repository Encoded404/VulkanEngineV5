module;

#include <cstdint>
#include <cstring>
#include <memory>

#include <vulkan/vulkan_raii.hpp>

module VulkanEngine.GpuTexture;

import VulkanEngine.Runtime.VulkanBootstrap;
import VulkanEngine.Utils.MemoryUtils;
import VulkanEngine.Utils.ImageUtils;

namespace VulkanEngine::GpuResources {

namespace {

void CreateBufferResource(const VulkanEngine::Runtime::IVulkanBootstrapBackend & backend,
                          uint64_t size,
                          vk::BufferUsageFlags usage,
                          vk::MemoryPropertyFlags properties,
                          std::unique_ptr<vk::raii::Buffer>& out_buffer,
                          std::unique_ptr<vk::raii::DeviceMemory>& out_memory) {
    vk::BufferCreateInfo const info({}, size, usage);
    out_buffer = std::make_unique<vk::raii::Buffer>(backend.GetDevice(), info);

    const vk::DeviceBufferMemoryRequirements mem_req_info{
        &info,
        nullptr
    };
    const vk::MemoryRequirements2 requirements = backend.GetDevice().getBufferMemoryRequirements(mem_req_info);
    const auto& reqs = requirements.memoryRequirements;

    vk::MemoryAllocateInfo const alloc(reqs.size,
        VulkanEngine::Utils::MemoryUtils::FindMemoryType(backend.GetPhysicalDevice(), reqs.memoryTypeBits, properties));
    out_memory = std::make_unique<vk::raii::DeviceMemory>(backend.GetDevice(), alloc);
    out_buffer->bindMemory(*out_memory, 0);
}

} // namespace

GpuTexture GpuTexture::CreateFromPixels(VulkanEngine::Runtime::IVulkanBootstrapBackend& backend,
                                        const uint8_t* pixels,
                                        uint32_t width,
                                        uint32_t height,
                                        vk::Format format) {
    GpuTexture texture{};
    texture.width_ = width;
    texture.height_ = height;

    const uint64_t pixel_size = static_cast<uint64_t>(width) * height * 4;

    std::unique_ptr<vk::raii::Buffer> staging_buffer;
    std::unique_ptr<vk::raii::DeviceMemory> staging_memory;
    CreateBufferResource(backend, pixel_size,
                         vk::BufferUsageFlagBits::eTransferSrc,
                         vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
                         staging_buffer, staging_memory);

    void* data = staging_memory->mapMemory(0, pixel_size);
    std::memcpy(data, pixels, pixel_size);
    staging_memory->unmapMemory();

    vk::ImageCreateInfo const image_info({}, vk::ImageType::e2D, format,
                                         vk::Extent3D(width, height, 1), 1, 1,
                                         vk::SampleCountFlagBits::e1,
                                         vk::ImageTiling::eOptimal,
                                         vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled);
    texture.image_ = std::make_unique<vk::raii::Image>(backend.GetDevice(), image_info);

    vk::MemoryRequirements const requirements = texture.image_->getMemoryRequirements();
    vk::MemoryAllocateInfo const alloc(requirements.size,
        VulkanEngine::Utils::MemoryUtils::FindMemoryType(backend.GetPhysicalDevice(), requirements.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal));
    texture.memory_ = std::make_unique<vk::raii::DeviceMemory>(backend.GetDevice(), alloc);
    texture.image_->bindMemory(*texture.memory_, 0);

    auto& cmd = backend.GetCommandBuffer(0);
    cmd.reset({});
    cmd.begin({vk::CommandBufferUsageFlagBits::eOneTimeSubmit});

    VulkanEngine::Utils::ImageUtils::CmdTransitionImageLayout(cmd,
        **texture.image_, vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
        vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);

    VulkanEngine::Utils::ImageUtils::CmdCopyBufferToImage(cmd,
        **staging_buffer, **texture.image_, width, height, vk::ImageLayout::eTransferDstOptimal);

    VulkanEngine::Utils::ImageUtils::CmdTransitionImageLayout(cmd,
        **texture.image_, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
        vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);

    cmd.end();
    vk::SubmitInfo const submit(0, nullptr, nullptr, 1, &*cmd);
    backend.GetGraphicsQueue().submit(submit, nullptr);
    backend.GetGraphicsQueue().waitIdle();

    vk::ImageViewCreateInfo const view_info({}, **texture.image_, vk::ImageViewType::e2D, format,
                                            {}, {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1});
    texture.image_view_ = std::make_unique<vk::raii::ImageView>(backend.GetDevice(), view_info);

    vk::SamplerCreateInfo const sampler_info({}, vk::Filter::eLinear, vk::Filter::eLinear,
                                             vk::SamplerMipmapMode::eLinear,
                                             vk::SamplerAddressMode::eRepeat, vk::SamplerAddressMode::eRepeat,
                                             vk::SamplerAddressMode::eRepeat);
    texture.sampler_ = std::make_unique<vk::raii::Sampler>(backend.GetDevice(), sampler_info);

    return texture;
}

} // namespace VulkanEngine::GpuResources
