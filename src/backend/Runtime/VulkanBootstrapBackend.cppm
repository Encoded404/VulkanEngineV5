module;

#include <memory>

export module VulkanBackend.Runtime.VulkanBootstrapBackend;

import VulkanBackend.Runtime.VulkanBootstrap;

export namespace VulkanEngine::Runtime {

[[nodiscard]] std::shared_ptr<IVulkanBootstrapBackend> CreateVulkanBootstrapBackend();

}  // namespace VulkanEngine::Runtime
