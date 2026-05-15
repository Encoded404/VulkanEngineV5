module;

#include <cstdint>
#include <vulkan/vulkan_raii.hpp>

export module VulkanEngine.GpuBuffer;

export import VulkanEngine.Runtime.VulkanBootstrap;

export namespace VulkanEngine::GpuResources {

class GpuBuffer {
public:
    static GpuBuffer Create(VulkanEngine::Runtime::IVulkanBootstrapBackend& backend,
                            uint64_t size,
                            vk::BufferUsageFlags usage,
                            vk::MemoryPropertyFlags properties,
                            const void* initial_data = nullptr);

    void Upload(const void* data, uint64_t size);

    [[nodiscard]] vk::raii::Buffer& GetBuffer() { return *buffer_; }
    [[nodiscard]] const vk::raii::Buffer& GetBuffer() const { return *buffer_; }
    [[nodiscard]] vk::raii::DeviceMemory& GetMemory() { return *memory_; }
    [[nodiscard]] const vk::raii::DeviceMemory& GetMemory() const { return *memory_; }
    [[nodiscard]] uint64_t GetSize() const { return size_; }

private:
    std::unique_ptr<vk::raii::Buffer> buffer_;
    std::unique_ptr<vk::raii::DeviceMemory> memory_;
    uint64_t size_ = 0;
};

} // namespace VulkanEngine::GpuResources
