#include <volk.h>
#include <VulkanEngine/Utils/ImageUtils.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>


namespace VulkanEngine::Utils {

uint32_t ImageUtils::CalculateMipLevels(uint32_t width, uint32_t height, uint32_t depth) {
    const uint32_t max_dim = std::max({width, height, depth});
    return static_cast<uint32_t>(std::floor(std::log2(std::max(1u, max_dim)))) + 1;
}

VkImageAspectFlags ImageUtils::GetImageAspectFlags(VkFormat format) {
    switch (format) {
        case VK_FORMAT_D32_SFLOAT:
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_X8_D24_UNORM_PACK32:
            return VK_IMAGE_ASPECT_DEPTH_BIT;
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D16_UNORM_S8_UINT:
            return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        default:
            return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

VkImageSubresourceRange ImageUtils::CreateSubresourceRange(VkImageAspectFlags aspect_flags,
                                                            uint32_t base_mip_level,
                                                            uint32_t level_count,
                                                            uint32_t base_array_layer,
                                                            uint32_t layer_count) {
    VkImageSubresourceRange range{};
    range.aspectMask = aspect_flags;
    range.baseMipLevel = base_mip_level;
    range.levelCount = level_count;
    range.baseArrayLayer = base_array_layer;
    range.layerCount = layer_count;
    return range;
}

VkBufferImageCopy ImageUtils::CreateBufferImageCopy(uint32_t width,
                                                    uint32_t height,
                                                    VkImageAspectFlags aspect_flags,
                                                    uint32_t mip_level,
                                                    uint32_t array_layer,
                                                    VkDeviceSize buffer_offset) {
    VkBufferImageCopy copy{};
    copy.bufferOffset = buffer_offset;
    copy.bufferRowLength = 0;
    copy.bufferImageHeight = 0;
    copy.imageSubresource.aspectMask = aspect_flags;
    copy.imageSubresource.mipLevel = mip_level;
    copy.imageSubresource.baseArrayLayer = array_layer;
    copy.imageSubresource.layerCount = 1;
    copy.imageOffset = {0, 0, 0};
    copy.imageExtent = {width, height, 1};
    return copy;
}

VkImageCopy ImageUtils::CreateImageCopy(const VkImageSubresourceLayers& src_subresource,
                                         const VkOffset3D& src_offset,
                                         const VkImageSubresourceLayers& dst_subresource,
                                         const VkOffset3D& dst_offset,
                                         const VkExtent3D& extent) {
    VkImageCopy copy{};
    copy.srcSubresource = src_subresource;
    copy.srcOffset = src_offset;
    copy.dstSubresource = dst_subresource;
    copy.dstOffset = dst_offset;
    copy.extent = extent;
    return copy;
}

VkImageSubresourceLayers ImageUtils::CreateSubresourceLayers(VkImageAspectFlags aspect_flags,
                                                             uint32_t mip_level,
                                                             uint32_t base_array_layer,
                                                             uint32_t layer_count) {
    VkImageSubresourceLayers layers{};
    layers.aspectMask = aspect_flags;
    layers.mipLevel = mip_level;
    layers.baseArrayLayer = base_array_layer;
    layers.layerCount = layer_count;
    return layers;
}

VkDeviceSize ImageUtils::CalculateImageSize(uint32_t width,
                                            uint32_t height,
                                            uint32_t depth,
                                            uint32_t mip_levels,
                                            uint32_t array_layers,
                                            VkFormat /*format*/) {
    VkDeviceSize size = 0;
    for (uint32_t layer = 0; layer < array_layers; ++layer) {
        uint32_t mip_width = width;
        uint32_t mip_height = height;
        uint32_t mip_depth = depth;
        for (uint32_t level = 0; level < mip_levels; ++level) {
            size += static_cast<VkDeviceSize>(std::max(1u, mip_width) * std::max(1u, mip_height) * std::max(1u, mip_depth));
            mip_width = std::max(mip_width / 2, 1u);
            mip_height = std::max(mip_height / 2, 1u);
            mip_depth = std::max(mip_depth / 2, 1u);
        }
    }
    return size;
}

void ImageUtils::GetMipLevelDimensions(uint32_t base_mip_width,
                                       uint32_t base_mip_height,
                                       uint32_t base_mip_depth,
                                       uint32_t mip_level,
                                       uint32_t& mip_width,
                                       uint32_t& mip_height,
                                       uint32_t& mip_depth) {
    mip_width = std::max(1u, base_mip_width >> mip_level);
    mip_height = std::max(1u, base_mip_height >> mip_level);
    mip_depth = std::max(1u, base_mip_depth >> mip_level);
}

bool ImageUtils::RequiresOptimalTiling(VkImageUsageFlags usage) {
    return (usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                     VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                     VK_IMAGE_USAGE_STORAGE_BIT)) != 0;
}

VkImageViewType ImageUtils::GetCompatibleViewType(VkImageType image_type, uint32_t array_layers) {
    switch (image_type) {
        case VK_IMAGE_TYPE_1D:
            return array_layers > 1 ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
        case VK_IMAGE_TYPE_2D:
            return array_layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        case VK_IMAGE_TYPE_3D:
            return VK_IMAGE_VIEW_TYPE_3D;
        default:
            return VK_IMAGE_VIEW_TYPE_2D;
    }
}

VkImageViewCreateInfo ImageUtils::CreateImageViewCreateInfo(VkImage image,
                                                            VkImageViewType view_type,
                                                            VkFormat format,
                                                            VkImageAspectFlags aspect_flags,
                                                            uint32_t base_mip_level,
                                                            uint32_t level_count,
                                                            uint32_t base_array_layer,
                                                            uint32_t layer_count) {
    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = image;
    view_info.viewType = view_type;
    view_info.format = format;
    view_info.subresourceRange = CreateSubresourceRange(aspect_flags,
                                                       base_mip_level,
                                                       level_count,
                                                       base_array_layer,
                                                       layer_count);
    return view_info;
}

VkImageMemoryBarrier ImageUtils::CreateImageMemoryBarrier(VkImage image,
                                                          VkImageLayout old_layout,
                                                          VkImageLayout new_layout,
                                                          VkImageAspectFlags aspect_flags,
                                                          uint32_t base_mip_level,
                                                          uint32_t level_count,
                                                          uint32_t base_array_layer,
                                                          uint32_t layer_count,
                                                          uint32_t src_queue_family_index,
                                                          uint32_t dst_queue_family_index) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = src_queue_family_index;
    barrier.dstQueueFamilyIndex = dst_queue_family_index;
    barrier.image = image;
    barrier.subresourceRange = CreateSubresourceRange(aspect_flags,
                                                      base_mip_level,
                                                      level_count,
                                                      base_array_layer,
                                                      layer_count);
    return barrier;
}

VkPipelineStageFlags ImageUtils::GetLayoutPipelineStageFlags(VkImageLayout layout, bool is_source) {
    switch (layout) {
        case VK_IMAGE_LAYOUT_UNDEFINED:
            return is_source ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT;
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            return VK_PIPELINE_STAGE_TRANSFER_BIT;
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            return VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
            return VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            return VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        default:
            return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    }
}

VkAccessFlags ImageUtils::GetLayoutAccessFlags(VkImageLayout layout) {
    switch (layout) {
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            return VK_ACCESS_TRANSFER_WRITE_BIT;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            return VK_ACCESS_TRANSFER_READ_BIT;
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            return VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
            return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            return VK_ACCESS_SHADER_READ_BIT;
        default:
            return 0;
    }
}

void ImageUtils::CmdTransitionImageLayout(VkCommandBuffer cmd,
                                             VkImage image,
                                             VkImageLayout old_layout,
                                             VkImageLayout new_layout,
                                             VkImageAspectFlags aspect_flags,
                                             uint32_t base_mip_level,
                                             uint32_t level_count,
                                             uint32_t base_array_layer,
                                             uint32_t layer_count) {
    VkImageMemoryBarrier barrier = CreateImageMemoryBarrier(image,
                                                               old_layout,
                                                               new_layout,
                                                               aspect_flags,
                                                               base_mip_level,
                                                               level_count,
                                                               base_array_layer,
                                                               layer_count);
    barrier.srcAccessMask = GetLayoutAccessFlags(old_layout);
    barrier.dstAccessMask = GetLayoutAccessFlags(new_layout);
    const VkPipelineStageFlags src_stage = GetLayoutPipelineStageFlags(old_layout, /*is_source*/true);
    const VkPipelineStageFlags dst_stage = GetLayoutPipelineStageFlags(new_layout, /*is_source*/false);
    vkCmdPipelineBarrier(cmd,
                         src_stage, dst_stage,
                         0,
                         0, nullptr,
                         0, nullptr,
                         1, &barrier);
}

void ImageUtils::CmdCopyBufferToImage(VkCommandBuffer cmd,
                                          VkBuffer src_buffer,
                                          VkImage dst_image,
                                          uint32_t width,
                                          uint32_t height,
                                          VkImageLayout dst_layout,
                                          VkImageAspectFlags aspect_flags,
                                          uint32_t mip_level,
                                          uint32_t array_layer,
                                          VkDeviceSize buffer_offset) {
    const VkBufferImageCopy region = CreateBufferImageCopy(width, height, aspect_flags, mip_level, array_layer, buffer_offset);
    vkCmdCopyBufferToImage(cmd, src_buffer, dst_image, dst_layout, 1, &region);
}

} // namespace VulkanEngine::Utils
