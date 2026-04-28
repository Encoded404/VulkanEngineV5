#include <gtest/gtest.h>

#include <memory>
#include <cstdint>
#include <vector>
#include <vulkan/vulkan_raii.hpp>

import VulkanEngine.Runtime.VulkanBootstrap;

namespace {

using namespace VulkanEngine::Runtime;

class FakeVulkanBootstrapBackend final : public IVulkanBootstrapBackend {
public:
    bool instance_result = true; // NOLINT(misc-non-private-member-variables-in-classes)
    bool physical_device_result = true; // NOLINT(misc-non-private-member-variables-in-classes)
    bool logical_device_result = true; // NOLINT(misc-non-private-member-variables-in-classes)
    bool swapchain_result = true; // NOLINT(misc-non-private-member-variables-in-classes)
    bool shutdown_called = false; // NOLINT(misc-non-private-member-variables-in-classes)
    uint32_t produced_swapchain_image_count = 3; // NOLINT(misc-non-private-member-variables-in-classes)
    uint32_t produced_width = 1280; // NOLINT(misc-non-private-member-variables-in-classes)
    uint32_t produced_height = 720; // NOLINT(misc-non-private-member-variables-in-classes)
    uint32_t current_frames_in_flight = 2; // NOLINT(misc-non-private-member-variables-in-classes)

    [[nodiscard]] bool CreateInstance(const VulkanBootstrapConfig&) override { return instance_result; }
    [[nodiscard]] bool SelectPhysicalDevice() override { return physical_device_result; }
    [[nodiscard]] bool CreateLogicalDevice(uint32_t frames_in_flight) override {
        current_frames_in_flight = frames_in_flight;
        return logical_device_result;
    }
    [[nodiscard]] bool CreateSwapchain(uint32_t, uint32_t& out_image_count) override {
        out_image_count = produced_swapchain_image_count;
        return swapchain_result;
    }
    [[nodiscard]] bool GetSwapchainExtent(uint32_t& out_width, uint32_t& out_height) const override {
        out_width = produced_width;
        out_height = produced_height;
        return true;
    }

    // Unimplemented handle accessors for fake
    [[nodiscard]] const vk::raii::Instance& GetInstance() const override { throw std::runtime_error("Fake"); }
    [[nodiscard]] const vk::raii::PhysicalDevice& GetPhysicalDevice() const override { throw std::runtime_error("Fake"); }
    [[nodiscard]] const vk::raii::Device& GetDevice() const override { throw std::runtime_error("Fake"); }
    [[nodiscard]] const vk::raii::Queue& GetGraphicsQueue() const override { throw std::runtime_error("Fake"); }
    [[nodiscard]] uint32_t GetGraphicsQueueFamily() const override { return 0; }
    [[nodiscard]] const vk::raii::CommandPool& GetCommandPool() const override { throw std::runtime_error("Fake"); }
    [[nodiscard]] const vk::raii::Fence& GetInFlightFence(uint32_t) const override { throw std::runtime_error("Fake"); }
    [[nodiscard]] const vk::raii::Semaphore& GetImageAvailableSemaphore(uint32_t) const override { throw std::runtime_error("Fake"); }
    [[nodiscard]] const vk::raii::Semaphore& GetRenderFinishedSemaphore(uint32_t) const override { throw std::runtime_error("Fake"); }
    [[nodiscard]] vk::raii::CommandBuffer& GetCommandBuffer(uint32_t) override { throw std::runtime_error("Fake"); }

    [[nodiscard]] uint32_t GetFramesInFlight() const override { return current_frames_in_flight; }

    [[nodiscard]] const vk::raii::SwapchainKHR& GetSwapchain() const override { throw std::runtime_error("Fake"); }
    [[nodiscard]] const std::vector<vk::Image>& GetSwapchainImages() const override { static std::vector<vk::Image> dummy; return dummy; }
    [[nodiscard]] const std::vector<vk::raii::ImageView>& GetSwapchainImageViews() const override { static std::vector<vk::raii::ImageView> dummy; return dummy; }
    [[nodiscard]] std::vector<bool>& GetSwapchainImageInitializedFlags() override { static std::vector<bool> dummy; return dummy; }
    [[nodiscard]] const vk::SurfaceFormatKHR& GetSurfaceFormat() const override { static vk::SurfaceFormatKHR dummy; return dummy; }
    [[nodiscard]] vk::Format GetDepthFormat() const override { return vk::Format::eUndefined; }
    [[nodiscard]] const vk::raii::ImageView& GetDepthImageView() const override { throw std::runtime_error("Fake"); }
    [[nodiscard]] const vk::raii::Image& GetDepthImage() const override { throw std::runtime_error("Fake"); }

    [[nodiscard]] bool AcquireNextImage(uint32_t, uint32_t&) override { return true; }
    [[nodiscard]] bool Present(uint32_t, uint32_t, bool) override { return true; }

    void Shutdown() override { shutdown_called = true; }
};

TEST(VulkanBootstrapTest, InitializeBuildsRuntimeSkeletonState) {
    auto backend = std::make_shared<FakeVulkanBootstrapBackend>();
    VulkanBootstrap bootstrap(backend);
    ASSERT_TRUE(bootstrap.Initialize(VulkanBootstrapConfig{}));
    const auto snapshot = bootstrap.GetSnapshot();
    EXPECT_TRUE(snapshot.instance_ready);
    EXPECT_TRUE(snapshot.device_ready);
    EXPECT_TRUE(snapshot.swapchain_ready);
    EXPECT_EQ(snapshot.swapchain_image_count, 3u);
    EXPECT_EQ(snapshot.status, BootstrapStatus::Ok);
}

TEST(VulkanBootstrapTest, InitializeReportsInstanceFailure) {
    auto backend = std::make_shared<FakeVulkanBootstrapBackend>();
    backend->instance_result = false;
    VulkanBootstrap bootstrap(backend);
    EXPECT_FALSE(bootstrap.Initialize(VulkanBootstrapConfig{}));
    EXPECT_EQ(bootstrap.GetSnapshot().status, BootstrapStatus::InstanceCreationFailed);
}

TEST(VulkanBootstrapTest, OutOfDateCanBeRecoveredBySwapchainRecreate) {
    auto backend = std::make_shared<FakeVulkanBootstrapBackend>();
    VulkanBootstrap bootstrap(backend);
    ASSERT_TRUE(bootstrap.Initialize(VulkanBootstrapConfig{}));
    bootstrap.NotifySwapchainOutOfDate();
    EXPECT_EQ(bootstrap.BeginFrame().status, BootstrapStatus::SwapchainOutOfDate);
    backend->produced_swapchain_image_count = 4;
    ASSERT_TRUE(bootstrap.RecreateSwapchain());
    const auto frame = bootstrap.BeginFrame();
    EXPECT_EQ(frame.status, BootstrapStatus::Ok);
    EXPECT_EQ(frame.swapchain_image_count, 4u);
}

TEST(VulkanBootstrapTest, DeviceLostStatusPersistsUntilShutdown) {
    auto backend = std::make_shared<FakeVulkanBootstrapBackend>();
    VulkanBootstrap bootstrap(backend);
    ASSERT_TRUE(bootstrap.Initialize(VulkanBootstrapConfig{}));
    bootstrap.NotifyDeviceLost();
    EXPECT_EQ(bootstrap.BeginFrame().status, BootstrapStatus::DeviceLost);
    bootstrap.Shutdown();
    EXPECT_TRUE(backend->shutdown_called);
    EXPECT_FALSE(bootstrap.IsInitialized());
}

}  // namespace
