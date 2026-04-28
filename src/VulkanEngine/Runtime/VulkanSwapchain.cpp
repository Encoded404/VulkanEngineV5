module;

#include <vulkan/vulkan_raii.hpp>
#include <SDL3/SDL_video.h>
#include <cstdio>
#include <vector>
#include <algorithm>
#include <memory>

module VulkanEngine.Runtime.VulkanSwapchain;

import VulkanEngine.Runtime.VulkanDevice;
import VulkanEngine.Runtime.VulkanInstance;

namespace VulkanEngine::Runtime {

bool VulkanSwapchain::Initialize(const VulkanInstance& instance, const VulkanDevice& device, uint32_t preferred_image_count) {
    try {
        const auto& vk_surface = instance.GetSurface();
        const auto& vk_physical_device = device.GetPhysicalDevice();
        const auto& vk_device = device.GetDevice();
        SDL_Window* window = instance.GetWindow();

        const auto capabilities = vk_physical_device.getSurfaceCapabilitiesKHR(*vk_surface);
        const auto formats = vk_physical_device.getSurfaceFormatsKHR(*vk_surface);
        surface_format_ = formats.empty() ? vk::SurfaceFormatKHR{} : formats.front();

        const auto present_modes = vk_physical_device.getSurfacePresentModesKHR(*vk_surface);
        vk::PresentModeKHR present_mode = vk::PresentModeKHR::eFifo;
        if (std::ranges::find(present_modes, vk::PresentModeKHR::eMailbox) != present_modes.end()) {
            present_mode = vk::PresentModeKHR::eMailbox;
        }

        if (capabilities.currentExtent.width == UINT32_MAX || capabilities.currentExtent.height == UINT32_MAX) {
            int drawable_w = 0, drawable_h = 0;
            SDL_GetWindowSizeInPixels(window, &drawable_w, &drawable_h);
            swapchain_extent_.width = std::clamp(static_cast<uint32_t>(drawable_w), capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
            swapchain_extent_.height = std::clamp(static_cast<uint32_t>(drawable_h), capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
        } else {
            swapchain_extent_ = capabilities.currentExtent;
        }

        uint32_t image_count = std::max(preferred_image_count, capabilities.minImageCount);
        if (capabilities.maxImageCount > 0) {
            image_count = std::min(image_count, capabilities.maxImageCount);
        }

        vk::SwapchainCreateInfoKHR swap_info{};
        swap_info.surface = *vk_surface;
        swap_info.minImageCount = image_count;
        swap_info.imageFormat = surface_format_.format;
        swap_info.imageColorSpace = surface_format_.colorSpace;
        swap_info.imageExtent = swapchain_extent_;
        swap_info.imageArrayLayers = 1;
        swap_info.imageUsage = vk::ImageUsageFlagBits::eColorAttachment;
        swap_info.imageSharingMode = vk::SharingMode::eExclusive;
        swap_info.preTransform = capabilities.currentTransform;
        swap_info.compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
        swap_info.presentMode = present_mode;
        swap_info.clipped = VK_TRUE;

        swapchain_ = std::make_unique<vk::raii::SwapchainKHR>(vk_device, swap_info);
        swapchain_images_ = swapchain_->getImages();
        swapchain_image_initialized_.assign(swapchain_images_.size(), false);
        swapchain_image_count_ = static_cast<uint32_t>(swapchain_images_.size());

        if (!RebuildSwapchainViews(vk_device)) return false;
        return CreateDepthResources(vk_physical_device, vk_device);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "[VulkanSwapchain] initialization failed: %s\n", ex.what());
        return false;
    }
}

bool VulkanSwapchain::RebuildSwapchainViews(const vk::raii::Device& device) {
    try {
        swapchain_image_views_.clear();
        for (const auto& image : swapchain_images_) {
            vk::ImageViewCreateInfo view_info{};
            view_info.image = image;
            view_info.viewType = vk::ImageViewType::e2D;
            view_info.format = surface_format_.format;
            view_info.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
            view_info.subresourceRange.levelCount = 1;
            view_info.subresourceRange.layerCount = 1;
            swapchain_image_views_.emplace_back(device, view_info);
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool VulkanSwapchain::CreateDepthResources(const vk::raii::PhysicalDevice& physical_device, const vk::raii::Device& device) {
    try {
        vk::ImageCreateInfo image_info{};
        image_info.imageType = vk::ImageType::e2D;
        image_info.extent = vk::Extent3D{swapchain_extent_.width, swapchain_extent_.height, 1};
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.format = depth_format_;
        image_info.tiling = vk::ImageTiling::eOptimal;
        image_info.initialLayout = vk::ImageLayout::eUndefined;
        image_info.usage = vk::ImageUsageFlagBits::eDepthStencilAttachment;
        image_info.samples = vk::SampleCountFlagBits::e1;
        image_info.sharingMode = vk::SharingMode::eExclusive;

        depth_image_ = std::make_unique<vk::raii::Image>(device, image_info);

        const auto requirements = depth_image_->getMemoryRequirements();
        vk::MemoryAllocateInfo alloc_info{};
        alloc_info.allocationSize = requirements.size;

        const auto mem_properties = physical_device.getMemoryProperties();
        uint32_t memory_type_index = UINT32_MAX;
        for (uint32_t i = 0; i < mem_properties.memoryTypeCount; ++i) {
            if ((requirements.memoryTypeBits & (1u << i)) && (mem_properties.memoryTypes[i].propertyFlags & vk::MemoryPropertyFlagBits::eDeviceLocal)) {
                memory_type_index = i;
                break;
            }
        }
        if (memory_type_index == UINT32_MAX) return false;

        alloc_info.memoryTypeIndex = memory_type_index;

        depth_image_memory_ = std::make_unique<vk::raii::DeviceMemory>(device, alloc_info);
        depth_image_->bindMemory(**depth_image_memory_, 0);

        vk::ImageViewCreateInfo view_info{};
        view_info.image = **depth_image_;
        view_info.viewType = vk::ImageViewType::e2D;
        view_info.format = depth_format_;
        view_info.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eDepth;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.layerCount = 1;

        depth_image_view_ = std::make_unique<vk::raii::ImageView>(device, view_info);
        return true;
    } catch (...) {
        return false;
    }
}

void VulkanSwapchain::Shutdown() {
    depth_image_view_.reset();
    depth_image_memory_.reset();
    depth_image_.reset();
    swapchain_image_views_.clear();
    swapchain_images_.clear();
    swapchain_.reset();
}

bool VulkanSwapchain::Recreate(const VulkanInstance& instance, const VulkanDevice& device, uint32_t preferred_image_count) {
    Shutdown();
    return Initialize(instance, device, preferred_image_count);
}

} // namespace VulkanEngine::Runtime
