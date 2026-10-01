module;

export module VulkanEngine.GpuTexture;

import std;
import std.compat;

import vulkan_hpp;

export import VulkanBackend.Vulkan.VulkanBootstrap;

import VulkanEngine.TextureTypes;
import VulkanEngine.GpuResources.GpuImageHeap;

export namespace VulkanEngine::GpuResources {

// GPU-resident sampled texture. Images are sub-allocated from a GpuImageHeap
// (one allocator, one retire path); the view is created by the heap from the
// HeapViewDesc (view type + KTXswizzle components), the sampler is owned here.
class GpuTexture {
public:
    GpuTexture() = default;
    ~GpuTexture();
    GpuTexture(GpuTexture&& other) noexcept;
    GpuTexture& operator=(GpuTexture&& other) noexcept;
    GpuTexture(const GpuTexture&) = delete;
    GpuTexture& operator=(const GpuTexture&) = delete;

    // Subresource-complete upload from canonical TextureData. `resolved` is
    // the device format chosen by ResolveUploadFormat; per-subresource copies
    // are block-aligned and the final layout transition targets the fragment
    // and compute stages.
    static GpuTexture CreateFromTextureData(VulkanBackend::Vulkan::IVulkanBootstrap& backend,
                                            GpuImageHeap& heap,
                                            const VulkanEngine::Textures::TextureData& data,
                                            vk::Format resolved,
                                            const VulkanEngine::Textures::SamplerDesc& sampler_desc = {});

    // Allocates the image, view and sampler for a subresource-complete texture
    // WITHOUT uploading: the image starts in eUndefined and must be filled by a
    // recorded copy (see the async uploader, which records into the frame
    // command buffer). `resolved` is the device format, `data` supplies the
    // extent/mip/layer/swizzle metadata.
    static GpuTexture CreatePending(VulkanBackend::Vulkan::IVulkanBootstrap& backend,
                                    GpuImageHeap& heap,
                                    const VulkanEngine::Textures::TextureData& data,
                                    vk::Format resolved,
                                    const VulkanEngine::Textures::SamplerDesc& sampler_desc = {});

    // Convenience path for in-memory RGBA8 (fallback checkerboard, solid
    // colors): a single-subresource TextureData upload.
    static GpuTexture CreateFromPixels(VulkanBackend::Vulkan::IVulkanBootstrap& backend,
                                       GpuImageHeap& heap,
                                       const std::uint8_t* pixels,
                                       std::uint32_t width,
                                       std::uint32_t height,
                                       vk::Format format = vk::Format::eR8G8B8A8Unorm);

    // Persistent, GPU-resident image with no initial content; suitable as a
    // camera stream upload target (eTransferDst | eSampled, nearest sampler,
    // no mips). Images are updated in place each frame via vkCmdCopyBufferToImage.
    static GpuTexture CreateStream(VulkanBackend::Vulkan::IVulkanBootstrap& backend,
                                   GpuImageHeap& heap,
                                   std::uint32_t width,
                                   std::uint32_t height,
                                   vk::Format format,
                                   bool linear_filter = false);

    // Render target that can also be sampled by materials/shaders
    // (eColorAttachment | eSampled, linear sampler, no mips).
    static GpuTexture CreateColorTarget(VulkanBackend::Vulkan::IVulkanBootstrap& backend,
                                        GpuImageHeap& heap,
                                        std::uint32_t width,
                                        std::uint32_t height,
                                        vk::Format format = vk::Format::eR8G8B8A8Unorm);

    [[nodiscard]] vk::Image GetImage() const;
    [[nodiscard]] vk::ImageView GetImageView() const;
    [[nodiscard]] vk::Sampler GetSampler() const;
    [[nodiscard]] std::uint32_t GetWidth() const { return width_; }
    [[nodiscard]] std::uint32_t GetHeight() const { return height_; }
    [[nodiscard]] std::uint32_t GetMipLevels() const { return mip_levels_; }
    [[nodiscard]] std::uint32_t GetArrayLayers() const { return array_layers_; }
    [[nodiscard]] bool IsValid() const { return heap_ != nullptr && image_.IsValid(); }

    // Frame-gated destruction: hands the heap image to the heap's retire
    // ring; freed one FIF cycle after `recording_frame`. The sampler dies with
    // this object (samplers are immutable and cheap to replace).
    void Retire(std::uint32_t recording_frame);

private:
    GpuImageHeap* heap_ = nullptr;
    HeapImage image_{};
    std::unique_ptr<vk::raii::Sampler> sampler_{};
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::uint32_t mip_levels_ = 1;
    std::uint32_t array_layers_ = 1;
};

} // namespace VulkanEngine::GpuResources
