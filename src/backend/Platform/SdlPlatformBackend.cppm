module;

#include <memory>

export module VulkanEngine.Platform.SdlPlatformBackend;

import VulkanBackend.Platform.SdlPlatform;

export namespace VulkanEngine::Platform {

[[nodiscard]] std::shared_ptr<IPlatformBackend> CreateSdlPlatformBackend();

}  // namespace VulkanEngine::Platform
