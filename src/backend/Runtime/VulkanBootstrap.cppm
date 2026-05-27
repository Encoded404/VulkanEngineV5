module;

#include <memory>
#include <cstdint>
#include <vulkan/vulkan_raii.hpp>

export module VulkanBackend.Runtime.VulkanBootstrap;

export import VulkanBackend.Component;
export import VulkanBackend.Runtime.CommonTypes;

export namespace VulkanEngine::Runtime {

class IVulkanBootstrap {
public:
    virtual ~IVulkanBootstrap() = default;

    [[nodiscard]] virtual ComponentRegistry& GetComponentRegistry() = 0;
    [[nodiscard]] virtual const ComponentRegistry& GetComponentRegistry() const = 0;

    [[nodiscard]] virtual bool CreateInstance(const VulkanBootstrapConfig& config) = 0;
    [[nodiscard]] virtual bool SelectPhysicalDevice() = 0;
    [[nodiscard]] virtual bool CreateLogicalDevice(uint32_t frames_in_flight) = 0; // Modified
    [[nodiscard]] virtual bool CreateSwapchain(uint32_t preferred_image_count, PresentMode present_mode, uint32_t& out_image_count) = 0;
    [[nodiscard]] virtual bool GetSwapchainExtent(uint32_t& out_width, uint32_t& out_height) const = 0;

    // Core low-level access
    [[nodiscard]] virtual const vk::raii::Instance& GetInstance() const = 0;
    [[nodiscard]] virtual const vk::raii::PhysicalDevice& GetPhysicalDevice() const = 0;
    [[nodiscard]] virtual const vk::raii::Device& GetDevice() const = 0;
    [[nodiscard]] virtual const vk::raii::Queue& GetGraphicsQueue() const = 0;
    [[nodiscard]] virtual uint32_t GetGraphicsQueueFamily() const = 0;
    [[nodiscard]] virtual const vk::raii::CommandPool& GetCommandPool() const = 0;

    // Modified to accept frame_idx
    [[nodiscard]] virtual const vk::raii::Fence& GetInFlightFence(uint32_t frame_idx) const = 0;
    [[nodiscard]] virtual const vk::raii::Semaphore& GetImageAvailableSemaphore(uint32_t frame_idx) const = 0;
    [[nodiscard]] virtual const vk::raii::Semaphore& GetRenderFinishedSemaphore(uint32_t frame_idx) const = 0;
    [[nodiscard]] virtual vk::raii::CommandBuffer& GetCommandBuffer(uint32_t frame_idx) = 0; // Modified

    [[nodiscard]] virtual uint32_t GetFramesInFlight() const = 0; // New method

    // Swapchain access
    [[nodiscard]] virtual const vk::raii::SwapchainKHR& GetSwapchain() const = 0;
    [[nodiscard]] virtual const std::vector<vk::Image>& GetSwapchainImages() const = 0;
    [[nodiscard]] virtual const std::vector<vk::raii::ImageView>& GetSwapchainImageViews() const = 0;
    [[nodiscard]] virtual std::vector<bool>& GetSwapchainImageInitializedFlags() = 0;
    [[nodiscard]] virtual const vk::SurfaceFormatKHR& GetSurfaceFormat() const = 0;
    [[nodiscard]] virtual vk::Format GetDepthFormat() const = 0;
    [[nodiscard]] virtual const vk::raii::ImageView& GetDepthImageView(uint32_t image_index) const = 0;
    [[nodiscard]] virtual const vk::raii::Image& GetDepthImage(uint32_t image_index) const = 0;

    // RenderGraph support
    [[nodiscard]] virtual bool AcquireNextImage(uint32_t frame_idx, uint32_t& out_image_index) = 0; // Modified
    [[nodiscard]] virtual bool Present(uint32_t frame_idx, uint32_t image_index, bool rendering_succeeded) = 0; // Modified

    // DGC support
    [[nodiscard]] virtual bool IsDgcAvailable() const = 0;
    [[nodiscard]] virtual uint32_t GetMaxDgcSequenceCount() const = 0;
    [[nodiscard]] virtual uint32_t GetMinDgcBufferOffsetAlignment() const = 0;

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
    [[nodiscard]] bool AcquireNextImage(uint32_t& out_image_index);
    [[nodiscard]] bool Present(uint32_t image_index, bool rendering_succeeded); // Modified

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

}  // namespace VulkanEngine::Runtime
