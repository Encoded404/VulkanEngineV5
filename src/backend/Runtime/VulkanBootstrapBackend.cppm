module;

#include <memory>

export module VulkanEngine.Runtime.VulkanBootstrapBackend;

import VulkanEngine.Runtime.VulkanBootstrap;

export namespace VulkanEngine::Runtime {

[[nodiscard]] std::shared_ptr<IVulkanBootstrapBackend> CreateVulkanBootstrapBackend();

}  // namespace VulkanEngine::Runtime
