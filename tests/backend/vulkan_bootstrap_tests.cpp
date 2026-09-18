#include <gtest/gtest.h>


import std;
import std.compat;

import vulkan_hpp;
import VulkanBackend.Vulkan.VulkanBootstrap;
import test_vulkan_fakes;

namespace {

using namespace VulkanBackend::Vulkan;
using TestSupport::FakeVulkanBootstrapBackend;

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
