module;

export module VulkanEngine.GpuTexture;

import std;
import std.compat;

import vulkan_hpp;

export import VulkanBackend.Runtime.VulkanBootstrap;

export namespace VulkanEngine::GpuResources {

class GpuTexture {
public:
    static GpuTexture CreateFromPixels(VulkanEngine::Runtime::IVulkanBootstrap& backend,
                                       const std::uint8_t* pixels,
                                       std::uint32_t width,
                                       std::uint32_t height,
                                       vk::Format format = vk::Format::eR8G8B8A8Unorm);

    [[nodiscard]] vk::raii::Image& GetImage() { return *image_; }
    [[nodiscard]] const vk::raii::Image& GetImage() const { return *image_; }
    [[nodiscard]] vk::raii::ImageView& GetImageView() { return *image_view_; }
    [[nodiscard]] const vk::raii::ImageView& GetImageView() const { return *image_view_; }
    [[nodiscard]] vk::raii::Sampler& GetSampler() { return *sampler_; }
    [[nodiscard]] const vk::raii::Sampler& GetSampler() const { return *sampler_; }
    [[nodiscard]] std::uint32_t GetWidth() const { return width_; }
    [[nodiscard]] std::uint32_t GetHeight() const { return height_; }
    [[nodiscard]] bool IsValid() const { return image_ != nullptr; }

private:
    std::unique_ptr<vk::raii::Image> image_;
    std::unique_ptr<vk::raii::DeviceMemory> memory_;
    std::unique_ptr<vk::raii::ImageView> image_view_;
    std::unique_ptr<vk::raii::Sampler> sampler_;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
};

} // namespace VulkanEngine::GpuResources
