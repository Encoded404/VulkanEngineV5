module;

#include <memory>

export module VulkanBackend.Runtime.VulkanBootstrapBackend;

import VulkanBackend.Runtime.VulkanBootstrap;

export namespace VulkanEngine::Runtime {

[[nodiscard]] std::shared_ptr<IVulkanBootstrap> CreateVulkanBootstrapBackend();

}  // namespace VulkanEngine::Runtime
