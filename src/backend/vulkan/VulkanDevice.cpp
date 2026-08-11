module;

#include <logging/logging_macros.hpp>

#include <vulkan/vulkan_hpp_macros.hpp>

module VulkanBackend.Vulkan.VulkanDevice;

import std;
import std.compat;

import logiface;

import vulkan_hpp;

import VulkanBackend.Vulkan.VulkanInstance;
import VulkanBackend.Vulkan.CommonTypes;
import VulkanBackend.Vulkan.VulkanCapabilities;
import VulkanBackend.Vulkan.VulkanDebugUtils;

namespace VulkanBackend::Vulkan {

bool VulkanDevice::SelectPhysicalDevice(const VulkanInstance& instance) {
    if (physical_device_) return true;

    instance_ = &instance;

    try {
        const auto& vk_instance = instance.GetInstance();
        const auto& vk_surface = instance.GetSurface();

        const auto physical_devices = vk_instance.enumeratePhysicalDevices();

        VulkanCapabilitiesBuilder builder{capabilities_, supported_};
        std::string report;

        for (const auto& device : physical_devices) {
            // Properties first: device name + apiVersion floor.
            auto props2 = device.getProperties2<vk::PhysicalDeviceProperties2,
                                                 vk::PhysicalDeviceDescriptorIndexingProperties,
                                                 vk::PhysicalDeviceDriverProperties>();
            supported_.properties = props2.get<vk::PhysicalDeviceProperties2>().properties;
            supported_.descriptor_indexing = props2.get<vk::PhysicalDeviceDescriptorIndexingProperties>();
            supported_.driver_properties = props2.get<vk::PhysicalDeviceDriverProperties>();

            const auto device_name = std::string(supported_.properties.deviceName);

            // Queue families: need one family with graphics AND present.
            std::optional<std::uint32_t> gfx_family;
            const auto queue_families = device.getQueueFamilyProperties();
            for (std::uint32_t i = 0; i < queue_families.size(); ++i) {
                const bool supports_graphics = (queue_families[i].queueFlags & vk::QueueFlagBits::eGraphics) != vk::QueueFlags{};
                const bool supports_present = device.getSurfaceSupportKHR(i, *vk_surface) == vk::True;
                if (supports_graphics && supports_present) {
                    gfx_family = i;
                    break;
                }
            }
            if (!gfx_family) {
                report += std::string(report.empty() ? "" : "; ") + "[" + device_name + "] no graphics+present queue family";
                continue;
            }

            std::string failures;

            // apiVersion floor (engine requires 1.3).
            if (supported_.properties.apiVersion < vk::ApiVersion13) {
                failures += "apiVersion < 1.3";
            }

            // Required device extensions.
            supported_.extensions = device.enumerateDeviceExtensionProperties();
            for (std::size_t i = 0; i < static_cast<std::size_t>(DeviceExtension::Count); ++i) {
                const auto& spec = kDeviceExtensionCatalog[i];
                if (spec.requirement == Requirement::Required && !supported_.HasExtension(spec.name)) {
                    if (!failures.empty()) failures += "; ";
                    failures += "missing required device extension " + std::string(spec.name);
                }
            }

            // Features2 query — one-time, chain written into the supported state.
            supported_.vulkan13.pNext = nullptr;
            supported_.vulkan12.pNext = &supported_.vulkan13;
            supported_.vulkan11.pNext = &supported_.vulkan12;
            supported_.features2.pNext = &supported_.vulkan11;
            if (supported_.HasExtension("VK_EXT_graphics_pipeline_library")) {
                supported_.vulkan13.pNext = &supported_.gpl;
            }
            (*device).getFeatures2(&supported_.features2);

            // Required features.
            for (std::size_t i = 0; i < static_cast<std::size_t>(Feature::Count); ++i) {
                const auto& spec = kFeatureCatalog[i];
                if (spec.requirement == Requirement::Required && !builder.IsFeatureSupported(static_cast<Feature>(i))) {
                    if (!failures.empty()) failures += "; ";
                    failures += "missing required feature " + std::string(spec.name);
                }
            }

            if (!failures.empty()) {
                report += std::string(report.empty() ? "" : "; ") + "[" + device_name + "] " + failures;
                continue;
            }

            physical_device_ = std::make_unique<vk::raii::PhysicalDevice>(vk_instance, *device);
            graphics_queue_family_ = *gfx_family;
            break;
        }

        if (!physical_device_) {
            std::string message = "no suitable physical device found";
            if (!report.empty()) {
                message += ": " + report;
            }
            builder.SetRequirementsUnmet();
            builder.AppendError(message);
            LOGIFACE_LOG(error, message);
            return false;
        }

    } catch (const std::exception& ex) {
        LOGIFACE_LOG(error, std::string("physical device selection failed: ") + ex.what());
        return false;
    }

    return true;
}

bool VulkanDevice::CreateLogicalDeviceAndResources(const std::uint32_t frames_in_flight,
                                                   const VulkanBootstrapConfig& config) {
    if (device_) return true;
    if (!physical_device_) return false;

    frames_in_flight_ = frames_in_flight;

    VulkanCapabilitiesBuilder builder{capabilities_, supported_};

    const auto is_force_disabled = [&config](std::string_view name) {
        for (const auto& disabled : config.force_disabled_extensions) {
            if (disabled == name) return true;
        }
        return false;
    };

    // 1. API version floor confirmation + config check.
    if (config.api_minor < 3) {
        builder.SetRequirementsUnmet();
        builder.AppendError("config api_minor < 3 (engine floor is 1.3)");
    }
    if (supported_.properties.apiVersion < vk::ApiVersion13) {
        builder.SetRequirementsUnmet();
        builder.AppendError("device apiVersion < 1.3");
    }

    // 3. Engine catalog device entries.
    const bool instance_debug_utils = instance_ &&
        instance_->GetCapabilities().IsInstanceExtensionEnabled(InstanceExtension::DebugUtils);

    std::vector<DeviceExtension> requested_extensions;
    std::vector<std::string> requested_dynamic_names;

    const auto try_request = [&](DeviceExtension e, std::string_view name, Requirement requirement) {
        if (!supported_.HasExtension(name)) {
            if (requirement == Requirement::Required) {
                builder.SetRequirementsUnmet();
                builder.AppendError("required device extension not supported: " + std::string(name));
            } else {
                LOGIFACE_LOG(warn, "optional device extension unavailable, skipping: " + std::string(name));
            }
            return;
        }
        requested_extensions.push_back(e);
    };

    try_request(DeviceExtension::Swapchain, "VK_KHR_swapchain", Requirement::Required);
    // VK_EXT_debug_utils is registered as an instance extension in the current
    // Vulkan registry (vk.xml type="instance"); its device-level commands
    // (vkSetDebugUtilsObjectNameEXT, labels) are resolved from the instance
    // enablement. A driver that still advertises it at device level predates
    // that clarification, so only request it there for compatibility with such
    // drivers — and warn about them instead of warning on modern drivers
    // (e.g. RADV) that correctly omit it.
    if (instance_debug_utils && supported_.HasExtension("VK_EXT_debug_utils")) {
        LOGIFACE_LOG(warn, "outdated driver: VK_EXT_debug_utils advertised as a device extension "
                           "(instance-only in current spec); requesting it for compatibility, please update your drivers for improved performance and compatibility.");
        requested_extensions.push_back(DeviceExtension::DebugUtils);
    }
    try_request(DeviceExtension::PipelineLibrary, "VK_KHR_pipeline_library", Requirement::Optional);
    if (builder.IsFeatureSupported(Feature::GraphicsPipelineLibrary)) {
        try_request(DeviceExtension::GraphicsPipelineLibrary, "VK_EXT_graphics_pipeline_library", Requirement::Optional);
    } else {
        LOGIFACE_LOG(warn, "graphics pipeline library feature not supported, using monolithic pipeline path");
    }

    // 4. App requests (deduped against the catalog; unknown names are dynamic).
    for (const auto& request : config.device_extensions) {
        bool in_catalog = false;
        for (std::size_t i = 0; i < static_cast<std::size_t>(DeviceExtension::Count); ++i) {
            if (kDeviceExtensionCatalog[i].name == request.name) {
                in_catalog = true;
                break;
            }
        }
        if (in_catalog) continue;
        if (!supported_.HasExtension(request.name)) {
            if (request.required) {
                builder.SetRequirementsUnmet();
                builder.AppendError("required device extension not supported: " + request.name);
            } else {
                LOGIFACE_LOG(warn, "optional device extension unavailable, skipping: " + request.name);
            }
            continue;
        }
        requested_dynamic_names.push_back(request.name);
    }

    // 5. Apply force-disabled extensions to the request set.
    for (auto it = requested_extensions.begin(); it != requested_extensions.end();) {
        const auto& spec = kDeviceExtensionCatalog[static_cast<std::size_t>(*it)];
        if (is_force_disabled(spec.name)) {
            LOGIFACE_LOG(warn, "force-disabled device extension: " + std::string(spec.name));
            it = requested_extensions.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = requested_dynamic_names.begin(); it != requested_dynamic_names.end();) {
        if (is_force_disabled(*it)) {
            LOGIFACE_LOG(warn, "force-disabled device extension: " + *it);
            it = requested_dynamic_names.erase(it);
        } else {
            ++it;
        }
    }

    // 6. Transitive dependency closure (respects force-disable + support),
    //    then drop dependents whose dependencies are missing (cascade).
    std::vector<bool> in_set(static_cast<std::size_t>(DeviceExtension::Count), false);
    for (const auto e : requested_extensions) {
        in_set[static_cast<std::size_t>(e)] = true;
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto e : requested_extensions) {
            for (const auto dep : kDeviceExtensionCatalog[static_cast<std::size_t>(e)].dependencies) {
                const std::size_t dep_index = static_cast<std::size_t>(dep);
                const auto& dep_spec = kDeviceExtensionCatalog[dep_index];
                if (!in_set[dep_index] && !is_force_disabled(dep_spec.name) && supported_.HasExtension(dep_spec.name)) {
                    in_set[dep_index] = true;
                    requested_extensions.push_back(dep);
                    changed = true;
                }
            }
        }
    }
    bool dropped = true;
    while (dropped) {
        dropped = false;
        for (auto it = requested_extensions.begin(); it != requested_extensions.end();) {
            const auto& spec = kDeviceExtensionCatalog[static_cast<std::size_t>(*it)];
            bool all_deps_present = true;
            for (const auto dep : spec.dependencies) {
                if (!in_set[static_cast<std::size_t>(dep)]) {
                    all_deps_present = false;
                    break;
                }
            }
            if (!all_deps_present) {
                LOGIFACE_LOG(warn, "dropping " + std::string(spec.name) + " (dependency unavailable)");
                in_set[static_cast<std::size_t>(*it)] = false;
                it = requested_extensions.erase(it);
                dropped = true;
            } else {
                ++it;
            }
        }
    }

    // 7. Feature requests — GPL feature is requested only when the GPL
    //    extension survives the final request set (keeps the create chain
    //    VUID-clean under force-disable).
    const bool gpl_enabled = in_set[static_cast<std::size_t>(DeviceExtension::GraphicsPipelineLibrary)];
    for (std::size_t i = 0; i < static_cast<std::size_t>(Feature::Count); ++i) {
        const auto feature = static_cast<Feature>(i);
        if (feature == Feature::GraphicsPipelineLibrary) {
            if (gpl_enabled) {
                builder.SetFeatureRequest(feature, /*required=*/false);
            }
            continue;
        }
        builder.SetFeatureRequest(feature, kFeatureCatalog[i].requirement == Requirement::Required);
    }

    // 8. Fail before creation if any required device requirement is unmet.
    if (capabilities_.HasUnmetRequirements()) {
        LOGIFACE_LOG(error, "device requirements unmet: " + capabilities_.GetErrorMessage());
        return false;
    }

    builder.SetDeviceExtensions(requested_extensions);
    builder.FinalizeFeatures();
    builder.SetProperties(supported_.properties);
    builder.SetDescriptorIndexingProperties(supported_.descriptor_indexing);
    builder.SetDriverProperties(supported_.driver_properties);
    for (const auto& name : requested_dynamic_names) {
        builder.AddDeviceExtensionName(name);
    }

    try {
        std::vector<const char*> device_extension_names;
        device_extension_names.reserve(requested_extensions.size() + requested_dynamic_names.size());
        for (const auto e : requested_extensions) {
            device_extension_names.push_back(kDeviceExtensionCatalog[static_cast<std::size_t>(e)].name.data());
        }
        for (const auto& name : requested_dynamic_names) {
            device_extension_names.push_back(name.c_str());
        }

        constexpr float queue_priority = 1.0f;
        vk::DeviceQueueCreateInfo queue_info{};
        queue_info.queueFamilyIndex = graphics_queue_family_;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &queue_priority;

        vk::DeviceCreateInfo device_info{};
        device_info.pNext = &builder.GetFeatureChain();
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue_info;
        device_info.enabledExtensionCount = static_cast<std::uint32_t>(device_extension_names.size());
        device_info.ppEnabledExtensionNames = device_extension_names.data();

        device_ = std::make_unique<vk::raii::Device>(*physical_device_, device_info);

        VULKAN_HPP_DEFAULT_DISPATCHER.init(**device_);

        graphics_queue_ = device_->getQueue(graphics_queue_family_, 0);

        vk::CommandPoolCreateInfo pool_info{};
        pool_info.queueFamilyIndex = graphics_queue_family_;
        pool_info.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
        command_pool_ = std::make_unique<vk::raii::CommandPool>(*device_, pool_info);
        VulkanBackend::Vulkan::SetVulkanObjectName(*device_, *command_pool_, "main-command-pool");

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
            VulkanBackend::Vulkan::SetVulkanObjectName(*device_, *image_available_semaphores_[i], "image-available-semaphore-" + std::to_string(i));
            in_flight_fences_[i] = std::make_unique<vk::raii::Fence>(*device_, fence_info);
            VulkanBackend::Vulkan::SetVulkanObjectName(*device_, *in_flight_fences_[i], "in-flight-fence-" + std::to_string(i));
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

    for (auto& fence : in_flight_fences_) {
        fence.reset();
    }
    for (auto& sem : image_available_semaphores_) {
        sem.reset();
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
    instance_ = nullptr;
    capabilities_ = VulkanCapabilities{};
    supported_ = SupportedDeviceState{};
}

} // namespace VulkanBackend::Vulkan
