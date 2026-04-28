module;

#include <vulkan/vulkan_raii.hpp>
#include <memory>
#include <vector>

export module VulkanEngine.Runtime.VulkanSwapchain;

import VulkanEngine.Runtime.VulkanDevice;
import VulkanEngine.Runtime.VulkanInstance;

export namespace VulkanEngine::Runtime {

class VulkanSwapchain {
public:
    [[nodiscard]] bool Initialize(const VulkanInstance& instance, const VulkanDevice& device, uint32_t preferred_image_count);
    void Shutdown();
    [[nodiscard]] bool Recreate(const VulkanInstance& instance, const VulkanDevice& device, uint32_t preferred_image_count);

    [[nodiscard]] uint32_t GetImageCount() const { return swapchain_image_count_; }
    [[nodiscard]] uint32_t GetWidth() const { return swapchain_extent_.width; }
    [[nodiscard]] uint32_t GetHeight() const { return swapchain_extent_.height; }
    [[nodiscard]] const vk::SurfaceFormatKHR& GetSurfaceFormat() const { return surface_format_; }
    [[nodiscard]] const vk::raii::SwapchainKHR& GetSwapchain() const { return *swapchain_; }
    [[nodiscard]] const std::vector<vk::Image>& GetImages() const { return swapchain_images_; }
    [[nodiscard]] const std::vector<vk::raii::ImageView>& GetImageViews() const { return swapchain_image_views_; }
    [[nodiscard]] std::vector<bool>& GetImageInitializedFlags() { return swapchain_image_initialized_; }

    // Depth buffer support
    [[nodiscard]] vk::Format GetDepthFormat() const { return depth_format_; }
    [[nodiscard]] const vk::raii::ImageView& GetDepthImageView() const { return *depth_image_view_; }
    [[nodiscard]] const vk::raii::Image& GetDepthImage() const { return *depth_image_; }

private:
    std::unique_ptr<vk::raii::SwapchainKHR> swapchain_{};
    std::vector<vk::Image> swapchain_images_{};
    std::vector<vk::raii::ImageView> swapchain_image_views_{};
    vk::SurfaceFormatKHR surface_format_{};
    vk::Extent2D swapchain_extent_{};
    uint32_t swapchain_image_count_ = 0;
    std::vector<bool> swapchain_image_initialized_{};

    // Depth buffer resources
    vk::Format depth_format_ = vk::Format::eD32Sfloat;
    std::unique_ptr<vk::raii::Image> depth_image_{};
    std::unique_ptr<vk::raii::DeviceMemory> depth_image_memory_{};
    std::unique_ptr<vk::raii::ImageView> depth_image_view_{};

    [[nodiscard]] bool RebuildSwapchainViews(const vk::raii::Device& device);
    [[nodiscard]] bool CreateDepthResources(const vk::raii::PhysicalDevice& physical_device, const vk::raii::Device& device);
};

} // namespace VulkanEngine::Runtime
