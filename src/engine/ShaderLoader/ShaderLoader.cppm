module;

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

#include <vulkan/vulkan_raii.hpp>

export module VulkanEngine.ShaderLoader;

export import VulkanBackend.Runtime.VulkanBootstrap;

export namespace VulkanEngine::ShaderLoader {

class ShaderLoader {
public:
    [[nodiscard]] static std::vector<uint32_t> LoadSpirv(const std::filesystem::path& path);
    [[nodiscard]] static vk::raii::ShaderModule CreateModule(VulkanEngine::Runtime::IVulkanBootstrapBackend& backend,
                                                             const std::filesystem::path& path);
    [[nodiscard]] static vk::raii::ShaderModule CreateModuleFromSpirv(VulkanEngine::Runtime::IVulkanBootstrapBackend& backend,
                                                                      std::span<const uint32_t> spirv);
};

} // namespace VulkanEngine::ShaderLoader
