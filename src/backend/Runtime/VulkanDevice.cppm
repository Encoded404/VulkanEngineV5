module;

#include <vulkan/vulkan_raii.hpp>
#include <memory>
#include <vector>

export module VulkanBackend.Runtime.VulkanDevice;

import VulkanBackend.Runtime.VulkanInstance;

export namespace VulkanEngine::Runtime {

class VulkanDevice {
public:
    // New method for physical device selection
    [[nodiscard]] bool SelectPhysicalDevice(const VulkanInstance& instance);
    // Modified method for logical device creation and resource setup
    [[nodiscard]] bool CreateLogicalDeviceAndResources(uint32_t frames_in_flight);
    void Shutdown();

    [[nodiscard]] bool IsValid() const { return device_ != nullptr; }

    [[nodiscard]] const vk::raii::PhysicalDevice& GetPhysicalDevice() const { return *physical_device_; }
    [[nodiscard]] const vk::raii::Device& GetDevice() const { return *device_; }
    [[nodiscard]] const vk::raii::Queue& GetGraphicsQueue() const { return graphics_queue_; }
    [[nodiscard]] uint32_t GetGraphicsQueueFamily() const { return graphics_queue_family_; }
    [[nodiscard]] const vk::raii::CommandPool& GetCommandPool() const { return *command_pool_; }

    [[nodiscard]] const vk::raii::Fence& GetInFlightFence(uint32_t frame_idx) const { return *in_flight_fences_[frame_idx % frames_in_flight_]; }
    [[nodiscard]] const vk::raii::Semaphore& GetImageAvailableSemaphore(uint32_t frame_idx) const { return *image_available_semaphores_[frame_idx % frames_in_flight_]; }
    [[nodiscard]] const vk::raii::Semaphore& GetRenderFinishedSemaphore(uint32_t frame_idx) const { return *render_finished_semaphores_[frame_idx % frames_in_flight_]; }

    [[nodiscard]] vk::raii::CommandBuffer& GetCommandBuffer(uint32_t frame_idx) { return command_buffers_[frame_idx % frames_in_flight_]; }

    [[nodiscard]] uint32_t GetFramesInFlight() const { return frames_in_flight_; }

private:
    std::unique_ptr<vk::raii::PhysicalDevice> physical_device_{};
    std::unique_ptr<vk::raii::Device> device_{};
    vk::raii::Queue graphics_queue_ = nullptr;
    uint32_t graphics_queue_family_ = 0;

    std::unique_ptr<vk::raii::CommandPool> command_pool_{};
    std::vector<vk::raii::CommandBuffer> command_buffers_{};

    std::vector<std::unique_ptr<vk::raii::Semaphore>> image_available_semaphores_{};
    std::vector<std::unique_ptr<vk::raii::Semaphore>> render_finished_semaphores_{};
    std::vector<std::unique_ptr<vk::raii::Fence>> in_flight_fences_{};

    uint32_t frames_in_flight_ = 0;
};

} // namespace VulkanEngine::Runtime
