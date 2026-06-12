module;

#include <SDL3/SDL_vulkan.h>
#include <memory>
#include <vector>

#include "logging/logging.hpp"

#include <vulkan/vulkan_hpp_macros.hpp>

module VulkanBackend.Runtime.VulkanInstance;

import vulkan_hpp;

import VulkanBackend.Utils.Timer;

namespace VulkanEngine::Runtime {

bool VulkanInstance::Initialize(const VulkanBootstrapConfig& config) {
    if (instance_) return true;

    window_ = config.native_window_handle;
    if (!window_) {
        LOGIFACE_LOG(error, "native window handle is null");
        return false;
    }

    loader_ = std::make_unique<vk::detail::DynamicLoader>();
    VULKAN_HPP_DEFAULT_DISPATCHER.init(*loader_);

    uint32_t extension_count = 0;
    const char* const* sdl_extensions = SDL_Vulkan_GetInstanceExtensions(&extension_count);
    if (!sdl_extensions) {
        LOGIFACE_LOG(error, "SDL_Vulkan_GetInstanceExtensions failed");
        return false;
    }

    std::vector<const char*> instance_extensions(sdl_extensions, sdl_extensions + extension_count);
    std::vector<const char*> instance_layers{};
    if (config.enable_validation) {
        instance_layers.push_back("VK_LAYER_KHRONOS_validation");
    }

    constexpr vk::ApplicationInfo app_info("VulkanEngineV5", 1, "VulkanEngineV5", 1, vk::ApiVersion12);
    const vk::InstanceCreateInfo instance_info({}, &app_info,
        static_cast<uint32_t>(instance_layers.size()), instance_layers.data(),
        static_cast<uint32_t>(instance_extensions.size()), instance_extensions.data());

    try {
        instance_ = std::make_unique<vk::raii::Instance>(vk::raii::Context{}, instance_info);
        VULKAN_HPP_DEFAULT_DISPATCHER.init(**instance_);
    } catch (const std::exception& ex) {
        LOGIFACE_LOG(error, "instance creation failed: " + std::string(ex.what()));
        instance_.reset();
        return false;
    }

    VkSurfaceKHR raw_surface = nullptr;
    if (!SDL_Vulkan_CreateSurface(window_, static_cast<VkInstance>(**instance_), nullptr, &raw_surface)) {
        LOGIFACE_LOG(error, "SDL_Vulkan_CreateSurface failed");
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
