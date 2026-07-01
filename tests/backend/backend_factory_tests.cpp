#include <gtest/gtest.h>

import VulkanBackend.Platform.SdlPlatformBackend;
import VulkanBackend.Runtime.VulkanBootstrapBackend;

namespace {

TEST(BackendFactoryTest, SdlFactoryReturnsBackendInstance) {
    const auto backend = VulkanBackend::Platform::CreateSdlPlatformBackend();
    EXPECT_NE(backend, nullptr);
}

TEST(BackendFactoryTest, VulkanFactoryReturnsBackendInstance) {
    const auto backend = VulkanBackend::Runtime::CreateVulkanBootstrapBackend();
    EXPECT_NE(backend, nullptr);
}

}  // namespace
