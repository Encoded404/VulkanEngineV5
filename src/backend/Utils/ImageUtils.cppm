module;

#include <vulkan/vulkan_raii.hpp>

export module VulkanBackend.Utils.ImageUtils;

export namespace VulkanEngine::Utils {

class ImageUtils {
public:
    static uint32_t CalculateMipLevels(uint32_t width, uint32_t height, uint32_t depth = 1);
    static vk::ImageAspectFlags GetImageAspectFlags(vk::Format format);
    static vk::ImageSubresourceRange CreateSubresourceRange(vk::ImageAspectFlags aspect_flags,
                                                          uint32_t base_mip_level = 0,
                                                          uint32_t level_count = VK_REMAINING_MIP_LEVELS,
                                                          uint32_t base_array_layer = 0,
                                                          uint32_t layer_count = VK_REMAINING_ARRAY_LAYERS);
    static vk::BufferImageCopy CreateBufferImageCopy(uint32_t width,
                                                   uint32_t height,
                                                   vk::ImageAspectFlags aspect_flags,
                                                   uint32_t mip_level = 0,
                                                   uint32_t array_layer = 0,
                                                   vk::DeviceSize buffer_offset = 0);

    static vk::ImageCopy CreateImageCopy(const vk::ImageSubresourceLayers& src_subresource,
                                      const vk::Offset3D& src_offset,
                                      const vk::ImageSubresourceLayers& dst_subresource,
                                      const vk::Offset3D& dst_offset,
                                      const vk::Extent3D& extent);

    static vk::ImageSubresourceLayers CreateSubresourceLayers(vk::ImageAspectFlags aspect_flags,
                                                           uint32_t mip_level = 0,
                                                           uint32_t base_array_layer = 0,
                                                           uint32_t layer_count = 1);

    static vk::DeviceSize CalculateImageSize(uint32_t width, uint32_t height, uint32_t depth,
                                          uint32_t mip_levels, uint32_t array_layers,
                                          vk::Format format);

    static void GetMipLevelDimensions(uint32_t base_mip_width, uint32_t base_mip_height, uint32_t base_mip_depth,
                                     uint32_t mip_level,
                                     uint32_t& mip_width, uint32_t& mip_height, uint32_t& mip_depth);

    static bool RequiresOptimalTiling(vk::ImageUsageFlags usage);
    static vk::ImageViewType GetCompatibleViewType(vk::ImageType image_type, uint32_t array_layers);

    static vk::ImageViewCreateInfo CreateImageViewCreateInfo(vk::Image image,
                                                          vk::ImageViewType view_type,
                                                          vk::Format format,
                                                          vk::ImageAspectFlags aspect_flags,
                                                          uint32_t base_mip_level = 0,
                                                          uint32_t level_count = VK_REMAINING_MIP_LEVELS,
                                                          uint32_t base_array_layer = 0,
                                                          uint32_t layer_count = VK_REMAINING_ARRAY_LAYERS);

    static vk::ImageMemoryBarrier CreateImageMemoryBarrier(vk::Image image,
                                                         vk::ImageLayout old_layout,
                                                         vk::ImageLayout new_layout,
                                                         vk::ImageAspectFlags aspect_flags,
                                                         uint32_t base_mip_level = 0,
                                                         uint32_t level_count = VK_REMAINING_MIP_LEVELS,
                                                         uint32_t base_array_layer = 0,
                                                         uint32_t layer_count = VK_REMAINING_ARRAY_LAYERS,
                                                         uint32_t src_queue_family_index = VK_QUEUE_FAMILY_IGNORED,
                                                         uint32_t dst_queue_family_index = VK_QUEUE_FAMILY_IGNORED);

    static vk::PipelineStageFlags GetLayoutPipelineStageFlags(vk::ImageLayout layout, bool is_source);
    static vk::AccessFlags GetLayoutAccessFlags(vk::ImageLayout layout);

    static void CmdTransitionImageLayout(vk::raii::CommandBuffer const& cmd,
                                         vk::Image image,
                                         vk::ImageLayout old_layout,
                                         vk::ImageLayout new_layout,
                                         vk::ImageAspectFlags aspect_flags,
                                         uint32_t base_mip_level = 0,
                                         uint32_t level_count = VK_REMAINING_MIP_LEVELS,
                                         uint32_t base_array_layer = 0,
                                         uint32_t layer_count = VK_REMAINING_ARRAY_LAYERS);

    static void CmdCopyBufferToImage(vk::raii::CommandBuffer const& cmd,
                                     vk::Buffer src_buffer,
                                     vk::Image dst_image,
                                     uint32_t width,
                                     uint32_t height,
                                     vk::ImageLayout dst_layout,
                                     vk::ImageAspectFlags aspect_flags = vk::ImageAspectFlagBits::eColor,
                                     uint32_t mip_level = 0,
                                     uint32_t array_layer = 0,
                                     vk::DeviceSize buffer_offset = 0);
};

} // namespace VulkanEngine::Utils
