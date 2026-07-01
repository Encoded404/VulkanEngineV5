module;

export module VulkanBackend.Runtime.VulkanBootstrapBackend;

import std;

import VulkanBackend.Runtime.VulkanBootstrap;

export namespace VulkanBackend::Runtime {

[[nodiscard]] std::shared_ptr<IVulkanBootstrap> CreateVulkanBootstrapBackend();

}  // namespace VulkanBackend::Runtime
