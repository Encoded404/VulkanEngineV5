module;

export module VulkanBackend.Vulkan.VulkanBootstrap;

import std;

import vulkan_hpp;

export import VulkanBackend.Vulkan.CommonTypes;
export import VulkanBackend.Vulkan.VulkanCapabilities;

export namespace VulkanBackend::Vulkan {

class IVulkanBootstrap {
public:
    virtual ~IVulkanBootstrap() = default;

    [[nodiscard]] virtual bool CreateInstance(const VulkanBootstrapConfig& config) = 0;
    [[nodiscard]] virtual bool SelectPhysicalDevice() = 0;
    [[nodiscard]] virtual bool CreateLogicalDevice(std::uint32_t frames_in_flight) = 0; // Modified
    [[nodiscard]] virtual bool CreateSwapchain(std::uint32_t preferred_image_count, PresentMode present_mode, std::uint32_t& out_image_count) = 0;
    [[nodiscard]] virtual bool GetSwapchainExtent(std::uint32_t& out_width, std::uint32_t& out_height) const = 0;

    // Core low-level access
    [[nodiscard]] virtual const vk::raii::Instance& GetInstance() const = 0;
    [[nodiscard]] virtual const vk::raii::PhysicalDevice& GetPhysicalDevice() const = 0;
    [[nodiscard]] virtual const vk::raii::Device& GetDevice() const = 0;
    [[nodiscard]] virtual const vk::raii::Queue& GetGraphicsQueue() const = 0;
    [[nodiscard]] virtual std::uint32_t GetGraphicsQueueFamily() const = 0;
    [[nodiscard]] virtual const vk::raii::CommandPool& GetCommandPool() const = 0;

    // Capabilities snapshot (device domain) + bootstrap error state
    [[nodiscard]] virtual const VulkanCapabilities& GetCapabilities() const = 0;
    [[nodiscard]] virtual const std::string& GetErrorMessage() const = 0;
    [[nodiscard]] virtual bool HasUnmetRequirements() const = 0;

    // Modified to accept frame_idx
    [[nodiscard]] virtual const vk::raii::Fence& GetInFlightFence(std::uint32_t frame_idx) const = 0;
    [[nodiscard]] virtual const vk::raii::Semaphore& GetImageAvailableSemaphore(std::uint32_t frame_idx) const = 0;
    [[nodiscard]] virtual const vk::raii::Semaphore& GetRenderFinishedSemaphore(std::uint32_t frame_idx) const = 0;
    [[nodiscard]] virtual vk::raii::CommandBuffer& GetCommandBuffer(std::uint32_t frame_idx) = 0; // Modified

    [[nodiscard]] virtual std::uint32_t GetFramesInFlight() const = 0; // New method

    // Swapchain access
    [[nodiscard]] virtual const vk::raii::SwapchainKHR& GetSwapchain() const = 0;
    [[nodiscard]] virtual const std::vector<vk::Image>& GetSwapchainImages() const = 0;
    [[nodiscard]] virtual const std::vector<vk::raii::ImageView>& GetSwapchainImageViews() const = 0;
    [[nodiscard]] virtual std::vector<bool>& GetSwapchainImageInitializedFlags() = 0;
    [[nodiscard]] virtual const vk::SurfaceFormatKHR& GetSurfaceFormat() const = 0;
    [[nodiscard]] virtual vk::Format GetDepthFormat() const = 0;
    [[nodiscard]] virtual const vk::raii::ImageView& GetDepthImageView(std::uint32_t image_index) const = 0;
    [[nodiscard]] virtual const vk::raii::Image& GetDepthImage(std::uint32_t image_index) const = 0;

    // RenderGraph support
    [[nodiscard]] virtual bool AcquireNextImage(std::uint32_t frame_idx, std::uint32_t& out_image_index) = 0; // Modified
    // Resets the per-slot fence and queues the frame's command buffer for GPU
    // execution. Does NOT present; the image stays app-owned until Present().
    [[nodiscard]] virtual bool SubmitFrame(std::uint32_t frame_idx, std::uint32_t image_index, bool rendering_succeeded) = 0;
    // Queues vkQueuePresentKHR for a previously submitted image. The present
    // waits on that image's render-finished semaphore (signaled by SubmitFrame).
    [[nodiscard]] virtual bool Present(std::uint32_t image_index) = 0;
    // Non-blocking query: has all command work of frame frame_idx completed?
    [[nodiscard]] virtual bool IsFrameComplete(std::uint32_t frame_idx) = 0;

    virtual void Shutdown() = 0;
};

class VulkanBootstrap {
public:
    explicit VulkanBootstrap(std::shared_ptr<IVulkanBootstrap> backend);

    [[nodiscard]] bool Initialize(const VulkanBootstrapConfig& config);
    void Shutdown();

    [[nodiscard]] VulkanBootstrapState BeginFrame();
    void EndFrame();

    void NotifySwapchainOutOfDate();
    [[nodiscard]] bool RecreateSwapchain();

    // RenderGraph support
    [[nodiscard]] bool AcquireNextImage(std::uint32_t& out_image_index);
    [[nodiscard]] bool SubmitFrame(std::uint32_t image_index, bool rendering_succeeded);
    [[nodiscard]] bool Present(std::uint32_t image_index);
    [[nodiscard]] bool IsFrameComplete(std::uint32_t frame_idx) const;

    // Direct access to backend for custom rendering
    [[nodiscard]] IVulkanBootstrap& GetBackend() { return *backend_; }

    void NotifyDeviceLost();

    [[nodiscard]] bool IsInitialized() const;
    [[nodiscard]] const VulkanBootstrapState& GetSnapshot() const;

private:
    std::shared_ptr<IVulkanBootstrap> backend_{};
    VulkanBootstrapConfig config_{};
    VulkanBootstrapState snapshot_{};
    BootstrapStatus pending_status_ = BootstrapStatus::Ok;
    bool initialized_ = false;
};

}  // namespace VulkanBackend::Vulkan
