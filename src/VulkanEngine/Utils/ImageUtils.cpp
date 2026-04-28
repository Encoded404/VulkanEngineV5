module;

#include <vulkan/vulkan_raii.hpp>
#include <algorithm>
#include <cmath>

module VulkanEngine.Utils.ImageUtils;

namespace VulkanEngine::Utils {

uint32_t ImageUtils::CalculateMipLevels(uint32_t width, uint32_t height, uint32_t depth) {
    const uint32_t max_dim = std::max({width, height, depth});
    return static_cast<uint32_t>(std::floor(std::log2(std::max(1u, max_dim)))) + 1;
}

vk::ImageAspectFlags ImageUtils::GetImageAspectFlags(vk::Format format) {
    switch (format) {
        case vk::Format::eD32Sfloat:
        case vk::Format::eD16Unorm:
        case vk::Format::eX8D24UnormPack32:
            return vk::ImageAspectFlagBits::eDepth;
        case vk::Format::eD32SfloatS8Uint:
        case vk::Format::eD24UnormS8Uint:
        case vk::Format::eD16UnormS8Uint:
            return vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil;
        default:
            return vk::ImageAspectFlagBits::eColor;
    }
}

vk::ImageSubresourceRange ImageUtils::CreateSubresourceRange(vk::ImageAspectFlags aspect_flags,
                                                            uint32_t base_mip_level,
                                                            uint32_t level_count,
                                                            uint32_t base_array_layer,
                                                            uint32_t layer_count) {
    vk::ImageSubresourceRange range{};
    range.aspectMask = aspect_flags;
    range.baseMipLevel = base_mip_level;
    range.levelCount = level_count;
    range.baseArrayLayer = base_array_layer;
    range.layerCount = layer_count;
    return range;
}

vk::BufferImageCopy ImageUtils::CreateBufferImageCopy(uint32_t width,
                                                    uint32_t height,
                                                    vk::ImageAspectFlags aspect_flags,
                                                    uint32_t mip_level,
                                                    uint32_t array_layer,
                                                    vk::DeviceSize buffer_offset) {
    vk::BufferImageCopy copy{};
    copy.bufferOffset = buffer_offset;
    copy.bufferRowLength = 0;
    copy.bufferImageHeight = 0;
    copy.imageSubresource.aspectMask = aspect_flags;
    copy.imageSubresource.mipLevel = mip_level;
    copy.imageSubresource.baseArrayLayer = array_layer;
    copy.imageSubresource.layerCount = 1;
    copy.imageOffset = vk::Offset3D{0, 0, 0};
    copy.imageExtent = vk::Extent3D{width, height, 1};
    return copy;
}

vk::ImageCopy ImageUtils::CreateImageCopy(const vk::ImageSubresourceLayers& src_subresource,
                                         const vk::Offset3D& src_offset,
                                         const vk::ImageSubresourceLayers& dst_subresource,
                                         const vk::Offset3D& dst_offset,
                                         const vk::Extent3D& extent) {
    vk::ImageCopy copy{};
    copy.srcSubresource = src_subresource;
    copy.srcOffset = src_offset;
    copy.dstSubresource = dst_subresource;
    copy.dstOffset = dst_offset;
    copy.extent = extent;
    return copy;
}

vk::ImageSubresourceLayers ImageUtils::CreateSubresourceLayers(vk::ImageAspectFlags aspect_flags,
                                                             uint32_t mip_level,
                                                             uint32_t base_array_layer,
                                                             uint32_t layer_count) {
    vk::ImageSubresourceLayers layers{};
    layers.aspectMask = aspect_flags;
    layers.mipLevel = mip_level;
    layers.baseArrayLayer = base_array_layer;
    layers.layerCount = layer_count;
    return layers;
}

vk::DeviceSize ImageUtils::CalculateImageSize(uint32_t width,
                                            uint32_t height,
                                            uint32_t depth,
                                            uint32_t mip_levels,
                                            uint32_t array_layers,
                                            vk::Format /*format*/) {
    vk::DeviceSize size = 0;
    for (uint32_t layer = 0; layer < array_layers; ++layer) {
        uint32_t mip_width = width;
        uint32_t mip_height = height;
        uint32_t mip_depth = depth;
        for (uint32_t level = 0; level < mip_levels; ++level) {
            size += static_cast<vk::DeviceSize>(std::max(1u, mip_width) * std::max(1u, mip_height) * std::max(1u, mip_depth));
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

bool ImageUtils::RequiresOptimalTiling(vk::ImageUsageFlags usage) {
    return (usage & (vk::ImageUsageFlagBits::eColorAttachment |
                     vk::ImageUsageFlagBits::eDepthStencilAttachment |
                     vk::ImageUsageFlagBits::eStorage)) != vk::ImageUsageFlags{};
}

vk::ImageViewType ImageUtils::GetCompatibleViewType(vk::ImageType image_type, uint32_t array_layers) {
    switch (image_type) {
        case vk::ImageType::e1D:
            return array_layers > 1 ? vk::ImageViewType::e1DArray : vk::ImageViewType::e1D;
        case vk::ImageType::e2D:
            return array_layers > 1 ? vk::ImageViewType::e2DArray : vk::ImageViewType::e2D;
        case vk::ImageType::e3D:
            return vk::ImageViewType::e3D;
        default:
            return vk::ImageViewType::e2D;
    }
}

vk::ImageViewCreateInfo ImageUtils::CreateImageViewCreateInfo(vk::Image image,
                                                            vk::ImageViewType view_type,
                                                            vk::Format format,
                                                            vk::ImageAspectFlags aspect_flags,
                                                              uint32_t base_mip_level,
                                                              uint32_t level_count,
                                                              uint32_t base_array_layer,
                                                              uint32_t layer_count) {
    vk::ImageViewCreateInfo view_info{};
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

vk::ImageMemoryBarrier ImageUtils::CreateImageMemoryBarrier(vk::Image image,
                                                          vk::ImageLayout old_layout,
                                                          vk::ImageLayout new_layout,
                                                          vk::ImageAspectFlags aspect_flags,
                                                            uint32_t base_mip_level,
                                                            uint32_t level_count,
                                                            uint32_t base_array_layer,
                                                            uint32_t layer_count,
                                                            uint32_t src_queue_family_index,
                                                            uint32_t dst_queue_family_index) {
    vk::ImageMemoryBarrier barrier{};
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

vk::PipelineStageFlags ImageUtils::GetLayoutPipelineStageFlags(vk::ImageLayout layout, bool is_source) {
    switch (layout) {
        case vk::ImageLayout::eUndefined:
            return is_source ? vk::PipelineStageFlagBits::eTopOfPipe : vk::PipelineStageFlagBits::eTransfer;
        case vk::ImageLayout::eTransferDstOptimal:
        case vk::ImageLayout::eTransferSrcOptimal:
            return vk::PipelineStageFlagBits::eTransfer;
        case vk::ImageLayout::eColorAttachmentOptimal:
            return vk::PipelineStageFlagBits::eColorAttachmentOutput;
        case vk::ImageLayout::eDepthStencilAttachmentOptimal:
            return vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests;
        case vk::ImageLayout::eShaderReadOnlyOptimal:
            return vk::PipelineStageFlagBits::eFragmentShader;
        default:
            return vk::PipelineStageFlagBits::eAllCommands;
    }
}

vk::AccessFlags ImageUtils::GetLayoutAccessFlags(vk::ImageLayout layout) {
    switch (layout) {
        case vk::ImageLayout::eTransferDstOptimal:
            return vk::AccessFlagBits::eTransferWrite;
        case vk::ImageLayout::eTransferSrcOptimal:
            return vk::AccessFlagBits::eTransferRead;
        case vk::ImageLayout::eColorAttachmentOptimal:
            return vk::AccessFlagBits::eColorAttachmentWrite;
        case vk::ImageLayout::eDepthStencilAttachmentOptimal:
            return vk::AccessFlagBits::eDepthStencilAttachmentWrite;
        case vk::ImageLayout::eShaderReadOnlyOptimal:
            return vk::AccessFlagBits::eShaderRead;
        default:
            return vk::AccessFlags{};
    }
}

void ImageUtils::CmdTransitionImageLayout(vk::raii::CommandBuffer const& cmd,
                                             vk::Image image,
                                             vk::ImageLayout old_layout,
                                             vk::ImageLayout new_layout,
                                             vk::ImageAspectFlags aspect_flags,
                                             uint32_t base_mip_level,
                                             uint32_t level_count,
                                             uint32_t base_array_layer,
                                             uint32_t layer_count) {
    vk::ImageMemoryBarrier barrier = CreateImageMemoryBarrier(image,
                                                               old_layout,
                                                               new_layout,
                                                               aspect_flags,
                                                               base_mip_level,
                                                               level_count,
                                                               base_array_layer,
                                                               layer_count);
    barrier.srcAccessMask = GetLayoutAccessFlags(old_layout);
    barrier.dstAccessMask = GetLayoutAccessFlags(new_layout);
    const vk::PipelineStageFlags src_stage = GetLayoutPipelineStageFlags(old_layout, /*is_source*/true);
    const vk::PipelineStageFlags dst_stage = GetLayoutPipelineStageFlags(new_layout, /*is_source*/false);
    cmd.pipelineBarrier(src_stage, dst_stage,
                         vk::DependencyFlags{},
                         nullptr,
                         nullptr,
                         barrier);
}

void ImageUtils::CmdCopyBufferToImage(vk::raii::CommandBuffer const& cmd,
                                          vk::Buffer src_buffer,
                                          vk::Image dst_image,
                                          uint32_t width,
                                          uint32_t height,
                                          vk::ImageLayout dst_layout,
                                          vk::ImageAspectFlags aspect_flags,
                                          uint32_t mip_level,
                                          uint32_t array_layer,
                                          vk::DeviceSize buffer_offset) {
    const vk::BufferImageCopy region = CreateBufferImageCopy(width, height, aspect_flags, mip_level, array_layer, buffer_offset);
    cmd.copyBufferToImage(src_buffer, dst_image, dst_layout, region);
}

} // namespace VulkanEngine::Utils
