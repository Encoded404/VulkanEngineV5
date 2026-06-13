module;

#include <logging/logging_macros.hpp>

#include <vulkan/vulkan_hpp_macros.hpp>

module VulkanBackend.Runtime.VulkanDevice;

import std;
import std.compat;

import logiface;

import vulkan_hpp;

import VulkanBackend.Runtime.VulkanInstance;
import VulkanBackend.Runtime.CommonTypes;
import VulkanBackend.Utils.VulkanDebugUtils;

namespace VulkanEngine::Runtime {

bool VulkanDevice::SelectPhysicalDevice(const VulkanInstance& instance) {
    if (physical_device_) return true;

    try {
        const auto& vk_instance = instance.GetInstance();
        const auto& vk_surface = instance.GetSurface();

        const auto physical_devices = vk_instance.enumeratePhysicalDevices();
        for (const auto& device : physical_devices) {
            const auto queue_families = device.getQueueFamilyProperties();
            for (std::uint32_t i = 0; i < queue_families.size(); ++i) {
                const bool supports_graphics = (queue_families[i].queueFlags & vk::QueueFlagBits::eGraphics) != vk::QueueFlags{};
                const bool supports_present = device.getSurfaceSupportKHR(i, *vk_surface) == vk::True;
                if (supports_graphics && supports_present) {
                    physical_device_ = std::make_unique<vk::raii::PhysicalDevice>(vk_instance, *device);
                    graphics_queue_family_ = i;
                    break;
                }
            }
            if (physical_device_) break;
        }

        if (!physical_device_) {
            LOGIFACE_LOG(error, "failed to find suitable physical device");
            return false;
        }

    } catch (const std::exception& ex) {
        LOGIFACE_LOG(error, std::string("physical device selection failed: ") + ex.what());
        return false;
    }

    return true;
}

bool VulkanDevice::CreateLogicalDeviceAndResources(const std::uint32_t frames_in_flight) {
    if (device_) return true;
    if (!physical_device_) return false;

    frames_in_flight_ = frames_in_flight;

    // Query DGC properties
    {
        vk::PhysicalDeviceDeviceGeneratedCommandsPropertiesEXT dgc_props{};
        dgc_props.sType = vk::StructureType::ePhysicalDeviceDeviceGeneratedCommandsFeaturesEXT;
        vk::PhysicalDeviceProperties2 props2{};
        props2.sType = vk::StructureType::ePhysicalDeviceFeatures2;
        props2.pNext = &dgc_props;
        static_cast<vk::PhysicalDevice>(**physical_device_).getProperties2(&props2);
        if (dgc_props.maxIndirectSequenceCount >= 1 &&
            dgc_props.maxIndirectCommandsTokenCount >= 2 &&
            dgc_props.maxIndirectCommandsIndirectStride >= 20 &&
            (dgc_props.supportedIndirectCommandsShaderStagesPipelineBinding & vk::ShaderStageFlagBits::eVertex)) {
            dgc_available_ = true;
            max_dgc_sequence_count_ = std::min(dgc_props.maxIndirectSequenceCount, 256u);
        } else {
            LOGIFACE_LOG(debug, std::string("DGC not available: missing vertex shader stage support via pipeline binding.") +
                                            std::string("\nsupported = ") + std::bitset<32>(static_cast<std::uint32_t>(dgc_props.supportedIndirectCommandsShaderStagesPipelineBinding)).to_string() +
                                            std::string("\nneeded    = ") + std::bitset<32>(static_cast<int>(vk::ShaderStageFlagBits::eVertex)).to_string());
        }
    }

    // Check if DGC extension and its dependencies are available
    bool has_dgc_extension = false;
    if (dgc_available_) {
        bool has_maintenance5 = false;
        auto available_extensions = physical_device_->enumerateDeviceExtensionProperties();
        for (const auto& ext : available_extensions) {
            if (strcmp(ext.extensionName, vk::EXTDeviceGeneratedCommandsExtensionName) == 0) {
                has_dgc_extension = true;
            }
            if (strcmp(ext.extensionName, "VK_KHR_maintenance5") == 0) {
                has_maintenance5 = true;
            }
        }
        if (!has_dgc_extension || !has_maintenance5) {
            dgc_available_ = false;
        }
    }

    try {
        constexpr float queue_priority = 1.0f;
        vk::DeviceQueueCreateInfo queue_info{};
        queue_info.queueFamilyIndex = graphics_queue_family_;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &queue_priority;

        std::vector<const char*> device_extensions{vk::KHRSwapchainExtensionName};
        if (dgc_available_) {
            device_extensions.push_back("VK_KHR_maintenance5");
            device_extensions.push_back(vk::EXTDeviceGeneratedCommandsExtensionName);
        }

        vk::PhysicalDeviceDeviceGeneratedCommandsFeaturesEXT dgc_features{};
        dgc_features.deviceGeneratedCommands = vk::True;

        vk::PhysicalDeviceVulkan11Features vulkan11_features{};
        vulkan11_features.shaderDrawParameters = vk::True;

        vk::PhysicalDeviceVulkan12Features vulkan12_features{};
        vulkan12_features.hostQueryReset = vk::True;
        vulkan12_features.descriptorIndexing = vk::True;
        vulkan12_features.shaderSampledImageArrayNonUniformIndexing = vk::True;
        vulkan12_features.shaderStorageImageArrayNonUniformIndexing = vk::True;
        vulkan12_features.shaderStorageBufferArrayNonUniformIndexing = vk::True;
        vulkan12_features.runtimeDescriptorArray = vk::True;
        vulkan12_features.descriptorBindingPartiallyBound = vk::True;
        vulkan12_features.descriptorBindingSampledImageUpdateAfterBind = vk::True;
        vulkan12_features.descriptorBindingStorageBufferUpdateAfterBind = vk::True;
        vulkan12_features.descriptorBindingVariableDescriptorCount = vk::True;
        vulkan12_features.bufferDeviceAddress = vk::True;

        vk::PhysicalDeviceVulkan13Features vulkan13_features{};
        vulkan13_features.dynamicRendering = vk::True;
        vulkan13_features.synchronization2 = vk::True;
        vulkan12_features.pNext = &vulkan13_features;

        if (dgc_available_) {
            vulkan13_features.pNext = &dgc_features;
        }

        vulkan11_features.pNext = &vulkan12_features;

        vk::PhysicalDeviceFeatures2 core_features{};
        core_features.features.multiDrawIndirect = vk::True;
        core_features.features.pipelineStatisticsQuery = vk::True;
        core_features.pNext = &vulkan11_features;

        vk::DeviceCreateInfo device_info{};
        device_info.pNext = &core_features;
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue_info;
        device_info.enabledExtensionCount = static_cast<std::uint32_t>(device_extensions.size());
        device_info.ppEnabledExtensionNames = device_extensions.data();

        // Mark DGC as unavailable if the extension was not enabled
        if (!has_dgc_extension) {
            dgc_available_ = false;
        }

        device_ = std::make_unique<vk::raii::Device>(*physical_device_, device_info);

        VULKAN_HPP_DEFAULT_DISPATCHER.init(**device_);

        graphics_queue_ = device_->getQueue(graphics_queue_family_, 0);

        vk::CommandPoolCreateInfo pool_info{};
        pool_info.queueFamilyIndex = graphics_queue_family_;
        pool_info.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
        command_pool_ = std::make_unique<vk::raii::CommandPool>(*device_, pool_info);
        VulkanEngine::Utils::SetVulkanObjectName(*device_, *command_pool_, "main-command-pool");

        vk::CommandBufferAllocateInfo cmd_alloc{};
        cmd_alloc.commandPool = **command_pool_;
        cmd_alloc.level = vk::CommandBufferLevel::ePrimary;
        cmd_alloc.commandBufferCount = frames_in_flight_;
        command_buffers_ = device_->allocateCommandBuffers(cmd_alloc);

        constexpr vk::SemaphoreCreateInfo sem_info{};
        vk::FenceCreateInfo fence_info{};
        fence_info.flags = vk::FenceCreateFlagBits::eSignaled;

        image_available_semaphores_.resize(frames_in_flight_);
        in_flight_fences_.resize(frames_in_flight_);

        for (std::uint32_t i = 0; i < frames_in_flight_; ++i) {
            image_available_semaphores_[i] = std::make_unique<vk::raii::Semaphore>(*device_, sem_info);
            VulkanEngine::Utils::SetVulkanObjectName(*device_, *image_available_semaphores_[i], "image-available-semaphore-" + std::to_string(i));
            in_flight_fences_[i] = std::make_unique<vk::raii::Fence>(*device_, fence_info);
            VulkanEngine::Utils::SetVulkanObjectName(*device_, *in_flight_fences_[i], "in-flight-fence-" + std::to_string(i));
        }

    } catch (const std::exception& ex) {
        LOGIFACE_LOG(error, std::string("logical device creation failed: ") + ex.what());
        Shutdown();
        return false;
    }

    return true;
}

void VulkanDevice::Shutdown() {
    if (device_) {
        device_->waitIdle();
    }

    for (std::uint32_t i = 0; i < frames_in_flight_; ++i) {
        in_flight_fences_[i].reset();
        image_available_semaphores_[i].reset();
    }

    image_available_semaphores_.clear();
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
