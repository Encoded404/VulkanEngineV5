module;

export module VulkanBackend.Platform.SdlPlatformBackend;

import std;

import VulkanBackend.Platform.SdlPlatform;

export namespace VulkanEngine::Platform {

[[nodiscard]] std::shared_ptr<IPlatformBackend> CreateSdlPlatformBackend();

}  // namespace VulkanEngine::Platform
