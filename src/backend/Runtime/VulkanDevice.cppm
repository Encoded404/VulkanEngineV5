module;

export module VulkanBackend.Runtime.VulkanDevice;

import std;

import vulkan_hpp;

import VulkanBackend.Runtime.VulkanInstance;

export namespace VulkanEngine::Runtime {

class VulkanDevice {
public:
    // New method for physical device selection
    [[nodiscard]] bool SelectPhysicalDevice(const VulkanInstance& instance);
    // Modified method for logical device creation and resource setup
    [[nodiscard]] bool CreateLogicalDeviceAndResources(std::uint32_t frames_in_flight);
    void Shutdown();

    [[nodiscard]] bool IsValid() const { return device_ != nullptr; }

    [[nodiscard]] const vk::raii::PhysicalDevice& GetPhysicalDevice() const { return *physical_device_; }
    [[nodiscard]] const vk::raii::Device& GetDevice() const { return *device_; }
    [[nodiscard]] const vk::raii::Queue& GetGraphicsQueue() const { return graphics_queue_; }
    [[nodiscard]] std::uint32_t GetGraphicsQueueFamily() const { return graphics_queue_family_; }
    [[nodiscard]] const vk::raii::CommandPool& GetCommandPool() const { return *command_pool_; }

    [[nodiscard]] const vk::raii::Fence& GetInFlightFence(std::uint32_t frame_idx) const { return *in_flight_fences_[frame_idx % frames_in_flight_]; }
    [[nodiscard]] const vk::raii::Semaphore& GetImageAvailableSemaphore(std::uint32_t frame_idx) const { return *image_available_semaphores_[frame_idx % frames_in_flight_]; }
    [[nodiscard]] vk::raii::CommandBuffer& GetCommandBuffer(std::uint32_t frame_idx) { return command_buffers_[frame_idx % frames_in_flight_]; }

    [[nodiscard]] std::uint32_t GetFramesInFlight() const { return frames_in_flight_; }

    [[nodiscard]] bool IsDgcAvailable() const { return dgc_available_; }
    [[nodiscard]] std::uint32_t GetMaxDgcSequenceCount() const { return max_dgc_sequence_count_; }
    [[nodiscard]] std::uint32_t GetMinDgcBufferOffsetAlignment() const { return 4u; }

private:
    std::unique_ptr<vk::raii::PhysicalDevice> physical_device_{};
    std::unique_ptr<vk::raii::Device> device_{};
    vk::raii::Queue graphics_queue_ = nullptr;
    std::uint32_t graphics_queue_family_ = 0;

    std::unique_ptr<vk::raii::CommandPool> command_pool_{};
    std::vector<vk::raii::CommandBuffer> command_buffers_{};

    std::vector<std::unique_ptr<vk::raii::Semaphore>> image_available_semaphores_{};
    std::vector<std::unique_ptr<vk::raii::Fence>> in_flight_fences_{};

    std::uint32_t frames_in_flight_ = 0;

    bool dgc_available_ = false;
    std::uint32_t max_dgc_sequence_count_ = 0;
};

} // namespace VulkanEngine::Runtime
