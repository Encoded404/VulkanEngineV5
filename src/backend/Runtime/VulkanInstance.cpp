module;

#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan_raii.hpp>
#include <SDL3/SDL_vulkan.h>
#include <cstdio>
#include <memory>
#include <vector>

#include "logging/logging.hpp"

module VulkanBackend.Runtime.VulkanInstance;

import VulkanBackend.Utils.Timer;

namespace VulkanEngine::Runtime {

bool VulkanInstance::Initialize(const VulkanBootstrapConfig& config) {
    if (instance_) return true;

    window_ = config.native_window_handle;
    if (!window_) {
        std::fprintf(stderr, "[VulkanInstance] native window handle is null\n");
        return false;
    }

    loader_ = std::make_unique<vk::detail::DynamicLoader>();
    auto vkGetInstanceProcAddr = loader_->getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"); // NOLINT(readability-identifier-naming)
    if (!vkGetInstanceProcAddr) {
        std::fprintf(stderr, "[VulkanInstance] failed to resolve vkGetInstanceProcAddr\n");
        return false;
    }
    VULKAN_HPP_DEFAULT_DISPATCHER.init(vkGetInstanceProcAddr);

    uint32_t extension_count = 0;
    const char* const* sdl_extensions = SDL_Vulkan_GetInstanceExtensions(&extension_count);
    if (!sdl_extensions) {
        std::fprintf(stderr, "[VulkanInstance] SDL_Vulkan_GetInstanceExtensions failed\n");
        return false;
    }

    std::vector<const char*> instance_extensions(sdl_extensions, sdl_extensions + extension_count);
    std::vector<const char*> instance_layers{};
    if (config.enable_validation) {
        instance_layers.push_back("VK_LAYER_KHRONOS_validation");
    }

    constexpr vk::ApplicationInfo app_info("VulkanEngineV5", 1, "VulkanEngineV5", 1, VK_API_VERSION_1_3);
    const vk::InstanceCreateInfo instance_info({}, &app_info,
        static_cast<uint32_t>(instance_layers.size()), instance_layers.data(),
        static_cast<uint32_t>(instance_extensions.size()), instance_extensions.data());

    try {
        instance_ = std::make_unique<vk::raii::Instance>(vk::raii::Context{}, instance_info);
        VULKAN_HPP_DEFAULT_DISPATCHER.init(**instance_);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "[VulkanInstance] instance creation failed: %s\n", ex.what());
        instance_.reset();
        return false;
    }

    VkSurfaceKHR raw_surface = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(window_, static_cast<VkInstance>(**instance_), nullptr, &raw_surface)) {
        std::fprintf(stderr, "[VulkanInstance] SDL_Vulkan_CreateSurface failed\n");
        Shutdown();
        return false;
    }
    surface_ = std::make_unique<vk::raii::SurfaceKHR>(*instance_, raw_surface);

    return true;
}

void VulkanInstance::Shutdown() {
    const VulkanEngine::Utils::Timer t{true};
    surface_.reset();
    LOGIFACE_LOG(debug, "took " + std::to_string(t.ElapsedMs()) + " ms to destroy surface in VulkanInstance::Shutdown.");
    instance_.reset();
    LOGIFACE_LOG(debug, "took " + std::to_string(t.ElapsedMs()) + " ms to destroy instance in VulkanInstance::Shutdown.");
    loader_.reset();
    LOGIFACE_LOG(debug, "took " + std::to_string(t.ElapsedMs()) + " ms to destroy loader in VulkanInstance::Shutdown.");
    window_ = nullptr;
}

} // namespace VulkanEngine::Runtime
