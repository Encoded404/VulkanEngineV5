#ifndef VULKAN_ENGINE_UTILS_IMAGE_UTILS_HPP
#define VULKAN_ENGINE_UTILS_IMAGE_UTILS_HPP

#include <volk.h>

namespace VulkanEngine::Utils {

class ImageUtils {
public:
    static uint32_t CalculateMipLevels(uint32_t width, uint32_t height, uint32_t depth = 1);
    static VkImageAspectFlags GetImageAspectFlags(VkFormat format);
    static VkImageSubresourceRange CreateSubresourceRange(VkImageAspectFlags aspect_flags,
                                                          uint32_t base_mip_level = 0,
                                                          uint32_t level_count = VK_REMAINING_MIP_LEVELS,
                                                          uint32_t base_array_layer = 0,
                                                          uint32_t layer_count = VK_REMAINING_ARRAY_LAYERS);
    static VkBufferImageCopy CreateBufferImageCopy(uint32_t width,
                                                   uint32_t height,
                                                   VkImageAspectFlags aspect_flags,
                                                   uint32_t mip_level = 0,
                                                   uint32_t array_layer = 0,
                                                   VkDeviceSize buffer_offset = 0);

    static VkImageCopy CreateImageCopy(const VkImageSubresourceLayers& src_subresource,
                                      const VkOffset3D& src_offset,
                                      const VkImageSubresourceLayers& dst_subresource,
                                      const VkOffset3D& dst_offset,
                                      const VkExtent3D& extent);

    static VkImageSubresourceLayers CreateSubresourceLayers(VkImageAspectFlags aspect_flags,
                                                           uint32_t mip_level = 0,
                                                           uint32_t base_array_layer = 0,
                                                           uint32_t layer_count = 1);

    static VkDeviceSize CalculateImageSize(uint32_t width, uint32_t height, uint32_t depth,
                                          uint32_t mip_levels, uint32_t array_layers,
                                          VkFormat format);

    static void GetMipLevelDimensions(uint32_t base_mip_width, uint32_t base_mip_height, uint32_t base_mip_depth,
                                     uint32_t mip_level,
                                     uint32_t& mip_width, uint32_t& mip_height, uint32_t& mip_depth);

    static bool RequiresOptimalTiling(VkImageUsageFlags usage);
    static VkImageViewType GetCompatibleViewType(VkImageType image_type, uint32_t array_layers);

    static VkImageViewCreateInfo CreateImageViewCreateInfo(VkImage image,
                                                          VkImageViewType view_type,
                                                          VkFormat format,
                                                          VkImageAspectFlags aspect_flags,
                                                          uint32_t base_mip_level = 0,
                                                          uint32_t level_count = VK_REMAINING_MIP_LEVELS,
                                                          uint32_t base_array_layer = 0,
                                                          uint32_t layer_count = VK_REMAINING_ARRAY_LAYERS);

    static VkImageMemoryBarrier CreateImageMemoryBarrier(VkImage image,
                                                         VkImageLayout old_layout,
                                                         VkImageLayout new_layout,
                                                         VkImageAspectFlags aspect_flags,
                                                         uint32_t base_mip_level = 0,
                                                         uint32_t level_count = VK_REMAINING_MIP_LEVELS,
                                                         uint32_t base_array_layer = 0,
                                                         uint32_t layer_count = VK_REMAINING_ARRAY_LAYERS,
                                                         uint32_t src_queue_family_index = VK_QUEUE_FAMILY_IGNORED,
                                                         uint32_t dst_queue_family_index = VK_QUEUE_FAMILY_IGNORED);

    static VkPipelineStageFlags GetLayoutPipelineStageFlags(VkImageLayout layout, bool is_source);
    static VkAccessFlags GetLayoutAccessFlags(VkImageLayout layout);

    static void CmdTransitionImageLayout(VkCommandBuffer cmd,
                                         VkImage image,
                                         VkImageLayout old_layout,
                                         VkImageLayout new_layout,
                                         VkImageAspectFlags aspect_flags,
                                         uint32_t base_mip_level = 0,
                                         uint32_t level_count = VK_REMAINING_MIP_LEVELS,
                                         uint32_t base_array_layer = 0,
                                         uint32_t layer_count = VK_REMAINING_ARRAY_LAYERS);

    static void CmdCopyBufferToImage(VkCommandBuffer cmd,
                                     VkBuffer src_buffer,
                                     VkImage dst_image,
                                     uint32_t width,
                                     uint32_t height,
                                     VkImageLayout dst_layout,
                                     VkImageAspectFlags aspect_flags = VK_IMAGE_ASPECT_COLOR_BIT,
                                     uint32_t mip_level = 0,
                                     uint32_t array_layer = 0,
                                     VkDeviceSize buffer_offset = 0);
};

} // namespace VulkanEngine::Utils

#endif // VULKAN_ENGINE_UTILS_IMAGE_UTILS_HPP
