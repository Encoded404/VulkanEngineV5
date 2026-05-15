module;

#include <vulkan/vulkan_raii.hpp>

#include <memory>
#include <vector>
//# include <cmath> clang tidy says its unused

#include <logging/logging.hpp>

module VulkanBackend.Runtime.VulkanBootstrapBackend;

import VulkanBackend.Component;
import VulkanBackend.Runtime.VulkanInstance;
import VulkanBackend.Runtime.VulkanDevice;
import VulkanBackend.Runtime.VulkanSwapchain;

namespace VulkanEngine::Runtime {

namespace {

class RaiiVulkanBootstrapBackend final : public IVulkanBootstrapBackend {
public:
    [[nodiscard]] ComponentRegistry& GetComponentRegistry() override { return component_registry_; }
    [[nodiscard]] const ComponentRegistry& GetComponentRegistry() const override { return component_registry_; }

    [[nodiscard]] bool CreateInstance(const VulkanBootstrapConfig& config) override {
        instance_ = std::make_unique<VulkanInstance>();
        return instance_->Initialize(config);
    }

    [[nodiscard]] bool SelectPhysicalDevice() override {
        if (!instance_) return false; // Ensure instance is initialized
        device_ = std::make_unique<VulkanDevice>();
        // Phase 1: Select the physical device.
        return device_->SelectPhysicalDevice(*instance_);
    }

    [[nodiscard]] bool CreateLogicalDevice(uint32_t frames_in_flight) override {
        if (!device_ || !instance_) return false; // Ensure device and instance are initialized
        // Phase 2: Create the logical device and all associated resources using the correct frame count.
        return device_->CreateLogicalDeviceAndResources(frames_in_flight);
    }

    [[nodiscard]] bool CreateSwapchain(const uint32_t preferred_image_count, uint32_t& out_image_count) override {
        if (device_ && device_->IsValid()) {
            device_->GetDevice().waitIdle();
        }
        swapchain_ = std::make_unique<VulkanSwapchain>();
        if (!swapchain_->Initialize(*instance_, *device_, preferred_image_count)) {
            swapchain_->Shutdown();
            swapchain_.reset();
            render_finished_semaphores_.clear();
            return false;
        }
        render_finished_semaphores_.clear();
        const auto image_count = swapchain_->GetImageCount();
        render_finished_semaphores_.reserve(image_count);
        const vk::SemaphoreCreateInfo sem_info{};
        for (uint32_t i = 0; i < image_count; ++i) {
            render_finished_semaphores_.push_back(std::make_unique<vk::raii::Semaphore>(device_->GetDevice(), sem_info));
        }
        out_image_count = swapchain_->GetImageCount();
        return true;
    }

    [[nodiscard]] bool GetSwapchainExtent(uint32_t& out_width, uint32_t& out_height) const override {
        if (!swapchain_) return false;
        out_width = swapchain_->GetWidth();
        out_height = swapchain_->GetHeight();
        return out_width > 0 && out_height > 0;
    }

    [[nodiscard]] const vk::raii::Instance& GetInstance() const override { return instance_->GetInstance(); }
    [[nodiscard]] const vk::raii::PhysicalDevice& GetPhysicalDevice() const override { return device_->GetPhysicalDevice(); }
    [[nodiscard]] const vk::raii::Device& GetDevice() const override { return device_->GetDevice(); }
    [[nodiscard]] const vk::raii::Queue& GetGraphicsQueue() const override { return device_->GetGraphicsQueue(); }
    [[nodiscard]] uint32_t GetGraphicsQueueFamily() const override { return device_->GetGraphicsQueueFamily(); }
    [[nodiscard]] const vk::raii::CommandPool& GetCommandPool() const override { return device_->GetCommandPool(); }

    [[nodiscard]] const vk::raii::Fence& GetInFlightFence(uint32_t frame_idx) const override { return device_->GetInFlightFence(frame_idx); }
    [[nodiscard]] const vk::raii::Semaphore& GetImageAvailableSemaphore(uint32_t frame_idx) const override { return device_->GetImageAvailableSemaphore(frame_idx); }
    [[nodiscard]] const vk::raii::Semaphore& GetRenderFinishedSemaphore(uint32_t image_idx) const override { return *render_finished_semaphores_.at(image_idx); }
    [[nodiscard]] vk::raii::CommandBuffer& GetCommandBuffer(uint32_t frame_idx) override { return device_->GetCommandBuffer(frame_idx); }

    [[nodiscard]] uint32_t GetFramesInFlight() const override { return device_->GetFramesInFlight(); }

    [[nodiscard]] const vk::raii::SwapchainKHR& GetSwapchain() const override { return swapchain_->GetSwapchain(); }
    [[nodiscard]] const std::vector<vk::Image>& GetSwapchainImages() const override { return swapchain_->GetImages(); }
    [[nodiscard]] const std::vector<vk::raii::ImageView>& GetSwapchainImageViews() const override { return swapchain_->GetImageViews(); }
    [[nodiscard]] std::vector<bool>& GetSwapchainImageInitializedFlags() override { return swapchain_->GetImageInitializedFlags(); }
    [[nodiscard]] const vk::SurfaceFormatKHR& GetSurfaceFormat() const override { return swapchain_->GetSurfaceFormat(); }
    [[nodiscard]] vk::Format GetDepthFormat() const override { return swapchain_->GetDepthFormat(); }
    [[nodiscard]] const vk::raii::ImageView& GetDepthImageView() const override { return swapchain_->GetDepthImageView(); }
    [[nodiscard]] const vk::raii::Image& GetDepthImage() const override { return swapchain_->GetDepthImage(); }

    [[nodiscard]] bool AcquireNextImage(uint32_t frame_idx, uint32_t& out_image_index) override {
        LOGIFACE_LOG(trace, "entering AcquireNextImage with frame index " + std::to_string(frame_idx));
        if (!device_ || !swapchain_) return false;

        const vk::raii::Device& vk_device = device_->GetDevice();
        const vk::raii::Fence& vk_in_flight_fence = device_->GetInFlightFence(frame_idx);
        const vk::raii::Semaphore& vk_image_available_semaphore = device_->GetImageAvailableSemaphore(frame_idx);

        constexpr uint64_t timeout = 1000000000; // 1 second

        // Wait for previous work to finish
        if (vk_device.waitForFences({*vk_in_flight_fence}, VK_TRUE, timeout) != vk::Result::eSuccess)
        {
            LOGIFACE_LOG(error, "Failed to wait for in-flight fence during AcquireNextImage.");
            return false;
        }

        vk::AcquireNextImageInfoKHR acquire_info{};
        acquire_info.swapchain = *swapchain_->GetSwapchain();
        acquire_info.timeout = timeout;
        acquire_info.semaphore = *vk_image_available_semaphore;
        acquire_info.deviceMask = 1;

        try {
            auto [Result, image_index] = vk_device.acquireNextImage2KHR(acquire_info);
            if (Result != vk::Result::eSuccess && Result != vk::Result::eSuboptimalKHR) {
                return false;
            }
            out_image_index = image_index;
        } catch (...) {
            LOGIFACE_LOG(error, "Failed to acquire next image during AcquireNextImage.");
            return false;
        }

        current_image_index_ = out_image_index;
        LOGIFACE_LOG(trace, "leaving AcquireNextImage successfully. image index is: " + std::to_string(out_image_index));
        return true;
    }

    [[nodiscard]] bool Present(uint32_t frame_idx, uint32_t image_index, bool rendering_succeeded) override {
        LOGIFACE_LOG(trace, "entering Present with frame index " + std::to_string(frame_idx) + " and image index " + std::to_string(image_index) + " and rendering succeeded " + std::to_string(rendering_succeeded) + ".");
        if (!device_ || !swapchain_) return false;

        const vk::raii::Device& vk_device = device_->GetDevice();
        const vk::raii::Queue& vk_graphics_queue = device_->GetGraphicsQueue();
        const vk::raii::CommandBuffer& vk_command_buffer = device_->GetCommandBuffer(frame_idx);
        const vk::raii::Semaphore& vk_image_available_semaphore = device_->GetImageAvailableSemaphore(frame_idx);
        const vk::raii::Semaphore& vk_render_finished_semaphore = GetRenderFinishedSemaphore(image_index);
        const vk::raii::Fence& vk_in_flight_fence = device_->GetInFlightFence(frame_idx);

        // Reset the fence ONLY when we are about to submit work.
        vk_device.resetFences({*vk_in_flight_fence});

        const vk::PipelineStageFlags wait_stage = vk::PipelineStageFlagBits::eColorAttachmentOutput;
        vk::SubmitInfo submit_info{};
        submit_info.waitSemaphoreCount = 1;
        submit_info.pWaitSemaphores = &*vk_image_available_semaphore;
        submit_info.pWaitDstStageMask = &wait_stage;

        if (rendering_succeeded) {
            submit_info.commandBufferCount = 1;
            submit_info.pCommandBuffers = &*vk_command_buffer;
            submit_info.signalSemaphoreCount = 1;
            submit_info.pSignalSemaphores = &*vk_render_finished_semaphore;
        } else {
            // If rendering didn't succeed, we still need to consume the image_available_semaphore
            // but we don't signal render_finished_semaphore as nothing will be presented.
            submit_info.commandBufferCount = 0;
            submit_info.pCommandBuffers = nullptr;
            submit_info.signalSemaphoreCount = 0;
            submit_info.pSignalSemaphores = nullptr;
        }

        vk_graphics_queue.submit({submit_info}, *vk_in_flight_fence);

        if (!rendering_succeeded) {
            LOGIFACE_LOG(trace, "Gracefully skipping presentation because rendering did not succeed.");
            return true;
        }

        vk::PresentInfoKHR present_info{};
        present_info.waitSemaphoreCount = 1;
        present_info.pWaitSemaphores = &*vk_render_finished_semaphore;
        present_info.swapchainCount = 1;
        present_info.pSwapchains = &*swapchain_->GetSwapchain();
        present_info.pImageIndices = &image_index;

        try {
            const vk::Result present = vk_graphics_queue.presentKHR(present_info);
            if (present == vk::Result::eErrorOutOfDateKHR || present == vk::Result::eSuboptimalKHR) {
                return false;
            }
            if (present != vk::Result::eSuccess) {
                return false;
            }
        } catch (...) {
            return false;
        }

        swapchain_->GetImageInitializedFlags()[image_index] = true;
        LOGIFACE_LOG(trace, "leaving Present successfully.");
        return true;
    }

    void Shutdown() override {
        if (device_ && device_->IsValid()) {
            device_->GetDevice().waitIdle();
        }

        render_finished_semaphores_.clear();
        if (swapchain_) {
            swapchain_->Shutdown();
        }
        if (device_) {
            device_->Shutdown();
        }
        if (instance_) {
            instance_->Shutdown();
        }
    }

private:
    ComponentRegistry component_registry_{};
    std::unique_ptr<VulkanInstance> instance_{};
    std::unique_ptr<VulkanDevice> device_{};
    std::unique_ptr<VulkanSwapchain> swapchain_{};
    std::vector<std::unique_ptr<vk::raii::Semaphore>> render_finished_semaphores_{};
    uint32_t current_image_index_ = 0;
};

}  // namespace

std::shared_ptr<IVulkanBootstrapBackend> CreateVulkanBootstrapBackend() {
    return std::make_shared<RaiiVulkanBootstrapBackend>();
}

}  // namespace VulkanEngine::Runtime
