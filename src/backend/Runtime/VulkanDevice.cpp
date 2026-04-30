module;

#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan_raii.hpp>
#include <cstdio>
#include <vector>
#include <array>
#include <memory>

module VulkanEngine.Runtime.VulkanDevice;

import VulkanEngine.Runtime.VulkanInstance;
import VulkanEngine.Runtime.CommonTypes;

namespace VulkanEngine::Runtime {

bool VulkanDevice::SelectPhysicalDevice(const VulkanInstance& instance) {
    if (physical_device_) return true;

    try {
        const auto& vk_instance = instance.GetInstance();
        const auto& vk_surface = instance.GetSurface();

        const auto physical_devices = vk_instance.enumeratePhysicalDevices();
        for (const auto& device : physical_devices) {
            const auto queue_families = device.getQueueFamilyProperties();
            for (uint32_t i = 0; i < queue_families.size(); ++i) {
                const bool supports_graphics = (queue_families[i].queueFlags & vk::QueueFlagBits::eGraphics) != vk::QueueFlags{};
                const bool supports_present = device.getSurfaceSupportKHR(i, static_cast<VkSurfaceKHR>(*vk_surface)) == VK_TRUE;
                if (supports_graphics && supports_present) {
                    physical_device_ = std::make_unique<vk::raii::PhysicalDevice>(vk_instance, static_cast<VkPhysicalDevice>(*device));
                    graphics_queue_family_ = i;
                    break;
                }
            }
            if (physical_device_) break;
        }

        if (!physical_device_) {
            std::fprintf(stderr, "[VulkanDevice] failed to find suitable physical device\n");
            return false;
        }

    } catch (const std::exception& ex) {
        std::fprintf(stderr, "[VulkanDevice] physical device selection failed: %s\n", ex.what());
        return false;
    }

    return true;
}

bool VulkanDevice::CreateLogicalDeviceAndResources(const uint32_t frames_in_flight) {
    if (device_) return true;
    if (!physical_device_) return false;

    frames_in_flight_ = frames_in_flight;

    try {
        constexpr float queue_priority = 1.0f;
        vk::DeviceQueueCreateInfo queue_info{};
        queue_info.queueFamilyIndex = graphics_queue_family_;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &queue_priority;

        constexpr std::array<const char*, 1> device_extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};

        vk::PhysicalDeviceVulkan13Features vulkan13_features{};
        vulkan13_features.dynamicRendering = VK_TRUE;
        vulkan13_features.synchronization2 = VK_TRUE;

        vk::DeviceCreateInfo device_info{};
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue_info;
        device_info.enabledExtensionCount = static_cast<uint32_t>(device_extensions.size());
        device_info.ppEnabledExtensionNames = device_extensions.data();
        device_info.pNext = &vulkan13_features;

        device_ = std::make_unique<vk::raii::Device>(*physical_device_, device_info);

        VULKAN_HPP_DEFAULT_DISPATCHER.init(**device_);

        graphics_queue_ = device_->getQueue(graphics_queue_family_, 0);

        vk::CommandPoolCreateInfo pool_info{};
        pool_info.queueFamilyIndex = graphics_queue_family_;
        pool_info.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
        command_pool_ = std::make_unique<vk::raii::CommandPool>(*device_, pool_info);

        vk::CommandBufferAllocateInfo cmd_alloc{};
        cmd_alloc.commandPool = static_cast<VkCommandPool>(**command_pool_);
        cmd_alloc.level = vk::CommandBufferLevel::ePrimary;
        cmd_alloc.commandBufferCount = frames_in_flight_;
        command_buffers_ = device_->allocateCommandBuffers(cmd_alloc);

        const vk::SemaphoreCreateInfo sem_info{};
        vk::FenceCreateInfo fence_info{};
        fence_info.flags = vk::FenceCreateFlagBits::eSignaled;

        image_available_semaphores_.resize(frames_in_flight_);
        render_finished_semaphores_.resize(frames_in_flight_);
        in_flight_fences_.resize(frames_in_flight_);

        for (uint32_t i = 0; i < frames_in_flight_; ++i) {
            image_available_semaphores_[i] = std::make_unique<vk::raii::Semaphore>(*device_, sem_info);
            render_finished_semaphores_[i] = std::make_unique<vk::raii::Semaphore>(*device_, sem_info);
            in_flight_fences_[i] = std::make_unique<vk::raii::Fence>(*device_, fence_info);
        }

    } catch (const std::exception& ex) {
        std::fprintf(stderr, "[VulkanDevice] logical device creation failed: %s\n", ex.what());
        Shutdown();
        return false;
    }

    return true;
}

void VulkanDevice::Shutdown() {
    if (device_) {
        device_->waitIdle();
    }

    for (uint32_t i = 0; i < frames_in_flight_; ++i) {
        in_flight_fences_[i].reset();
        render_finished_semaphores_[i].reset();
        image_available_semaphores_[i].reset();
    }

    image_available_semaphores_.clear();
    render_finished_semaphores_.clear();
    in_flight_fences_.clear();

    command_buffers_.clear(); // MUST clear buffers before the pool
    command_pool_.reset();
    device_.reset();
    physical_device_.reset();
    graphics_queue_ = nullptr;
    graphics_queue_family_ = 0;
    frames_in_flight_ = 0;
}

} // namespace VulkanEngine::Runtime
