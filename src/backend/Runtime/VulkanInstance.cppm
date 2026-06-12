module;

#include <SDL3/SDL_video.h>

export module VulkanBackend.Runtime.VulkanInstance;

import std;

import vulkan_hpp;

import VulkanBackend.Runtime.CommonTypes;

export namespace VulkanEngine::Runtime {

class VulkanInstance {
public:
    [[nodiscard]] bool Initialize(const VulkanBootstrapConfig& config);
    void Shutdown();

    [[nodiscard]] const vk::raii::Instance& GetInstance() const { return *instance_; }
    [[nodiscard]] const vk::raii::SurfaceKHR& GetSurface() const { return *surface_; }
    [[nodiscard]] SDL_Window* GetWindow() const { return window_; }

private:
    SDL_Window* window_ = nullptr;
    std::unique_ptr<vk::detail::DynamicLoader> loader_{};
    std::unique_ptr<vk::raii::Instance> instance_{};
    std::unique_ptr<vk::raii::SurfaceKHR> surface_{};
};

} // namespace VulkanEngine::Runtime
