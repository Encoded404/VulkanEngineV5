module;

export module VulkanEngine.ShaderLoader;

import std;
import std.compat;

import vulkan_hpp;

export import VulkanBackend.Runtime.VulkanBootstrap;

export namespace VulkanEngine::ShaderLoader {

class ShaderLoader {
public:
    [[nodiscard]] static std::vector<std::uint32_t> LoadSpirv(const std::filesystem::path& path);
    [[nodiscard]] static vk::raii::ShaderModule CreateModule(VulkanEngine::Runtime::IVulkanBootstrap& backend,
                                                             const std::filesystem::path& path);
    [[nodiscard]] static vk::raii::ShaderModule CreateModuleFromSpirv(VulkanEngine::Runtime::IVulkanBootstrap& backend,
                                                                      std::span<const std::uint32_t> spirv);
};

} // namespace VulkanEngine::ShaderLoader
