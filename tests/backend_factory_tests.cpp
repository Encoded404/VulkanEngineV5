#include <gtest/gtest.h>

import VulkanBackend.Platform.SdlPlatformBackend;
import VulkanBackend.Runtime.VulkanBootstrapBackend;

namespace {

TEST(BackendFactoryTest, SdlFactoryReturnsBackendInstance) {
    const auto backend = VulkanEngine::Platform::CreateSdlPlatformBackend();
    EXPECT_NE(backend, nullptr);
}

TEST(BackendFactoryTest, VulkanFactoryReturnsBackendInstance) {
    const auto backend = VulkanEngine::Runtime::CreateVulkanBootstrapBackend();
    EXPECT_NE(backend, nullptr);
}

}  // namespace
