module;

export module VulkanBackend.Vulkan.VulkanBootstrapBackend;

import std;

import VulkanBackend.Vulkan.VulkanBootstrap;

export namespace VulkanBackend::Vulkan {

[[nodiscard]] std::shared_ptr<IVulkanBootstrap> CreateVulkanBootstrapBackend();

}  // namespace VulkanBackend::Vulkan
