module;

#include <cstdint>
#include <cstring>
#include <memory>

#include <vulkan/vulkan_raii.hpp>

module VulkanEngine.GpuBuffer;

import VulkanEngine.Runtime.VulkanBootstrap;
import VulkanEngine.Utils.MemoryUtils;
import VulkanEngine.Utils.ImageUtils;

namespace VulkanEngine::GpuResources {

namespace {

void CreateBufferResource(VulkanEngine::Runtime::IVulkanBootstrapBackend& backend,
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

GpuBuffer GpuBuffer::Create(VulkanEngine::Runtime::IVulkanBootstrapBackend& backend,
                            uint64_t size,
                            vk::BufferUsageFlags usage,
                            vk::MemoryPropertyFlags properties,
                            const void* initial_data) {
    GpuBuffer buffer{};
    CreateBufferResource(backend, size, usage, properties, buffer.buffer_, buffer.memory_);
    buffer.size_ = size;

    if (initial_data) {
        buffer.Upload(initial_data, size);
    }

    return buffer;
}

void GpuBuffer::Upload(const void* data, uint64_t size) {
    void* mapped = memory_->mapMemory(0, size);
    std::memcpy(mapped, data, size);
    memory_->unmapMemory();
}

} // namespace VulkanEngine::GpuResources
