module;

#include <SDL3/SDL_video.h>

#include <logging/logging_macros.hpp>

module VulkanBackend.Vulkan.VulkanSwapchain;

import std;
import std.compat;
import logiface;

import vulkan;

import VulkanBackend.Vulkan.CommonTypes;
import VulkanBackend.Vulkan.VulkanDevice;
import VulkanBackend.Vulkan.VulkanInstance;
import VulkanBackend.Vulkan.VulkanCapabilities;
import VulkanBackend.Vulkan.VulkanDebugUtils;

namespace VulkanBackend::Vulkan {

namespace {

vk::PresentModeKHR ToVkPresentMode(PresentMode mode) {
    switch (mode) {
        case PresentMode::Mailbox:     return vk::PresentModeKHR::eMailbox;
        case PresentMode::Fifo:        return vk::PresentModeKHR::eFifo;
        case PresentMode::Immediate:   return vk::PresentModeKHR::eImmediate;
        case PresentMode::FifoRelaxed: return vk::PresentModeKHR::eFifoRelaxed;
        default:                       return vk::PresentModeKHR::eFifo;
    }
}

} // anonymous namespace

bool VulkanSwapchain::Initialize(const VulkanInstance& instance, const VulkanDevice& device,
                                 std::uint32_t preferred_image_count, PresentMode present_mode,
                                 const OutputSettings& output) {
    try {
        const auto& vk_surface = instance.GetSurface();
        const auto& vk_physical_device = device.GetPhysicalDevice();
        const auto& vk_device = device.GetDevice();
        SDL_Window* window = instance.GetWindow();

        const auto capabilities = vk_physical_device.getSurfaceCapabilitiesKHR(*vk_surface);
        const auto formats = vk_physical_device.getSurfaceFormatsKHR(*vk_surface);

        // Never take formats.front(): the compositor's order is a scanout preference, not a quality ranking.
        // On a KWin HDR session the first pair is R16G16B16A16_UNORM + SRGB_NONLINEAR, a 16-bit SDR surface; the selection below chooses by mode and transfer function instead.
        const auto format_supported = [&vk_physical_device](vk::Format format) {
            const auto properties = vk_physical_device.getFormatProperties(format);
            return static_cast<bool>(properties.optimalTilingFeatures &
                                     vk::FormatFeatureFlagBits::eColorAttachment);
        };
        const OutputTarget target =
            SelectOutputTarget(formats, output.mode, format_supported);
        surface_format_ = target.surface_format;
        if (!formats.empty() && surface_format_.format == vk::Format::eUndefined) {
            LOGIFACE_LOG(error, "no renderable swapchain surface format advertises a color space "
                                 "the engine can encode");
            return false;
        }
        LOGIFACE_LOG(info, std::string("swapchain output: ") +
                               vk::to_string(surface_format_.format) + " / " +
                               vk::to_string(surface_format_.colorSpace) +
                               " encoding=" + std::to_string(static_cast<int>(target.encoding)) +
                               (target.fallback ? " (fallback from requested mode)" : ""));

        const auto present_modes = vk_physical_device.getSurfacePresentModesKHR(*vk_surface);
        vk::PresentModeKHR vk_present_mode = ToVkPresentMode(present_mode);
        if (std::ranges::find(present_modes, vk_present_mode) == present_modes.end()) {
            vk_present_mode = vk::PresentModeKHR::eFifo;
        }

        if (capabilities.currentExtent.width == UINT32_MAX || capabilities.currentExtent.height == UINT32_MAX) {
            int drawable_w = 0, drawable_h = 0;
            SDL_GetWindowSizeInPixels(window, &drawable_w, &drawable_h);
            swapchain_extent_.width = std::clamp(static_cast<std::uint32_t>(drawable_w), capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
            swapchain_extent_.height = std::clamp(static_cast<std::uint32_t>(drawable_h), capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
        } else {
            swapchain_extent_ = capabilities.currentExtent;
        }

        std::uint32_t image_count = std::max(preferred_image_count, capabilities.minImageCount);
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
        // The engine's built-in passes only render into the backbuffer.
        // Keep eSampled for app passes that still sample it, but only when the selected format supports it: requesting an unsupported usage would fail swapchain creation on the format the picker just chose.
        vk::ImageUsageFlags image_usage = vk::ImageUsageFlagBits::eColorAttachment;
        if (vk_physical_device.getFormatProperties(surface_format_.format)
                .optimalTilingFeatures &
            vk::FormatFeatureFlagBits::eSampledImage) {
            image_usage |= vk::ImageUsageFlagBits::eSampled;
        }
        swap_info.imageUsage = image_usage;
        swap_info.imageSharingMode = vk::SharingMode::eExclusive;
        swap_info.preTransform = capabilities.currentTransform;
        swap_info.compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
        swap_info.presentMode = vk_present_mode;
        swap_info.clipped = vk::True;

        // HDR10 and scRGB need mastering metadata so the compositor can map the content instead of guessing.
        // The struct is only legal when VK_EXT_hdr_metadata is enabled; the shader still encodes PQ without it, but the display gets less information.
        vk::HdrMetadataEXT hdr_metadata{};
        bool apply_hdr_metadata = false;
        const bool hdr_output = target.encoding == OutputEncoding::Pq ||
                                target.encoding == OutputEncoding::Scrgb;
        if (hdr_output) {
            const float paper_white = std::max(output.paper_white_nits, 1.0f);
            const float peak = output.peak_nits > paper_white ? output.peak_nits : 1000.0f;
            if (device.GetCapabilities().IsDeviceExtensionEnabled(DeviceExtension::HdrMetadata)) {
                const bool pq = target.encoding == OutputEncoding::Pq;
                // HDR10 is mastered in Rec.2020; scRGB keeps the sRGB primaries.
                hdr_metadata.displayPrimaryRed =
                    pq ? vk::XYColorEXT{0.708f, 0.292f} : vk::XYColorEXT{0.640f, 0.330f};
                hdr_metadata.displayPrimaryGreen =
                    pq ? vk::XYColorEXT{0.170f, 0.797f} : vk::XYColorEXT{0.300f, 0.600f};
                hdr_metadata.displayPrimaryBlue =
                    pq ? vk::XYColorEXT{0.131f, 0.046f} : vk::XYColorEXT{0.150f, 0.060f};
                hdr_metadata.whitePoint = vk::XYColorEXT{0.3127f, 0.3290f};
                hdr_metadata.maxLuminance = peak;
                hdr_metadata.minLuminance = 0.005f;
                hdr_metadata.maxContentLightLevel = peak;
                hdr_metadata.maxFrameAverageLightLevel = peak * 0.5f;
                apply_hdr_metadata = true;
            } else {
                LOGIFACE_LOG(warn, "HDR output selected without VK_EXT_hdr_metadata; "
                                   "the display will tone-map without mastering luminance");
            }
        }

        swapchain_ = std::make_unique<vk::raii::SwapchainKHR>(vk_device, swap_info);
        VulkanBackend::Vulkan::SetVulkanObjectName(vk_device, *swapchain_, "swapchain");
        // HDR metadata is NOT a VkSwapchainCreateInfoKHR pNext structure; the extension attaches it to an existing swapchain through vkSetHdrMetadataEXT.
        if (apply_hdr_metadata) {
            const vk::Device raw_device = *vk_device;
            const vk::SwapchainKHR swapchain_handle = *swapchain_;
            raw_device.setHdrMetadataEXT(1, &swapchain_handle, &hdr_metadata);
        }
        swapchain_images_ = swapchain_->getImages();
        swapchain_image_initialized_.assign(swapchain_images_.size(), false);
        swapchain_image_count_ = static_cast<std::uint32_t>(swapchain_images_.size());

        if (!RebuildSwapchainViews(vk_device)) return false;
        return CreateDepthResources(vk_physical_device, vk_device);
    } catch (const std::exception& ex) {
        LOGIFACE_LOG(error, std::string("initialization failed: ") + ex.what());
        return false;
    }
}

bool VulkanSwapchain::RebuildSwapchainViews(const vk::raii::Device& device) {
    try {
        swapchain_image_views_.clear();
        std::uint32_t view_idx = 0;
        for (const auto& image : swapchain_images_) {
            vk::ImageViewCreateInfo view_info{};
            view_info.image = image;
            view_info.viewType = vk::ImageViewType::e2D;
            view_info.format = surface_format_.format;
            view_info.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
            view_info.subresourceRange.levelCount = 1;
            view_info.subresourceRange.layerCount = 1;
            swapchain_image_views_.emplace_back(device, view_info);
            VulkanBackend::Vulkan::SetVulkanObjectName(device, swapchain_image_views_.back(), "swapchain-image-view-" + std::to_string(view_idx++));
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool VulkanSwapchain::CreateDepthResources(const vk::raii::PhysicalDevice& physical_device, const vk::raii::Device& device) {
    try {
        const std::uint32_t count = swapchain_image_count_;

        depth_images_.clear();
        depth_image_memories_.clear();
        depth_image_views_.clear();
        depth_images_.reserve(count);
        depth_image_memories_.reserve(count);
        depth_image_views_.reserve(count);

        vk::ImageCreateInfo image_info{};
        image_info.imageType = vk::ImageType::e2D;
        image_info.extent = vk::Extent3D{swapchain_extent_.width, swapchain_extent_.height, 1};
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.format = depth_format_;
        image_info.tiling = vk::ImageTiling::eOptimal;
        image_info.initialLayout = vk::ImageLayout::eUndefined;
        image_info.usage = vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eSampled;
        image_info.samples = vk::SampleCountFlagBits::e1;
        image_info.sharingMode = vk::SharingMode::eExclusive;

        for (std::uint32_t i = 0; i < count; ++i) {
            depth_images_.emplace_back(device, image_info);
            VulkanBackend::Vulkan::SetVulkanObjectName(device, depth_images_.back(), "depth-image-" + std::to_string(i));

            const auto requirements = depth_images_.back().getMemoryRequirements();
            vk::MemoryAllocateInfo alloc_info{};
            alloc_info.allocationSize = requirements.size;

            const auto mem_properties = physical_device.getMemoryProperties();
            std::uint32_t memory_type_index = UINT32_MAX;
            for (std::uint32_t j = 0; j < mem_properties.memoryTypeCount; ++j) {
                if ((requirements.memoryTypeBits & (1u << j)) && (mem_properties.memoryTypes[j].propertyFlags & vk::MemoryPropertyFlagBits::eDeviceLocal)) {
                    memory_type_index = j;
                    break;
                }
            }
            if (memory_type_index == UINT32_MAX) return false;

            alloc_info.memoryTypeIndex = memory_type_index;
            depth_image_memories_.emplace_back(device, alloc_info);
            VulkanBackend::Vulkan::SetVulkanObjectName(device, depth_image_memories_.back(), "depth-memory-" + std::to_string(i));
            depth_images_.back().bindMemory(depth_image_memories_.back(), 0);

            vk::ImageViewCreateInfo view_info{};
            view_info.image = *depth_images_.back();
            view_info.viewType = vk::ImageViewType::e2D;
            view_info.format = depth_format_;
            view_info.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eDepth;
            view_info.subresourceRange.levelCount = 1;
            view_info.subresourceRange.layerCount = 1;

            depth_image_views_.emplace_back(device, view_info);
            VulkanBackend::Vulkan::SetVulkanObjectName(device, depth_image_views_.back(), "depth-image-view-" + std::to_string(i));
        }
        return true;
    } catch (...) {
        return false;
    }
}

void VulkanSwapchain::Shutdown() {
    depth_image_views_.clear();
    depth_image_memories_.clear();
    depth_images_.clear();
    swapchain_image_views_.clear();
    swapchain_images_.clear();
    swapchain_.reset();
}

bool VulkanSwapchain::Recreate(const VulkanInstance& instance, const VulkanDevice& device,
                               std::uint32_t preferred_image_count, PresentMode present_mode,
                               const OutputSettings& output) {
    Shutdown();
    return Initialize(instance, device, preferred_image_count, present_mode, output);
}

} // namespace VulkanBackend::Vulkan
