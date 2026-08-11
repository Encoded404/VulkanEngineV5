module;

export module VulkanEngine.ShaderManager;

import std;
import vulkan_hpp;
import ShaderReflection;
import VulkanBackend.Vulkan.VulkanCapabilities;

export namespace VulkanEngine::ShaderSystem {

using ShaderId = std::uint16_t;

struct ShaderModuleSlot {
    std::string spv_path;
    std::string slang_path;
    ShaderStage stage;
    std::vector<Binding> bindings;
    std::uint64_t binding_hash;
    vk::raii::ShaderModule module{nullptr};
    std::atomic<std::uint64_t> version{0};
    bool loaded{false};
    bool manual{false};
};

class ShaderManager {
public:
    ShaderManager(const vk::raii::Device& device, const VulkanBackend::Vulkan::VulkanCapabilities& caps,
                  std::string_view cache_dir);
    ~ShaderManager();

    ShaderManager(const ShaderManager&) = delete;
    ShaderManager& operator=(const ShaderManager&) = delete;
    ShaderManager(ShaderManager&&) = delete;
    ShaderManager& operator=(ShaderManager&&) = delete;

    ShaderId Register(std::string spv_path, std::string slang_path,
                      ShaderStage stage, std::span<const Binding> bindings,
                      std::uint64_t binding_hash);

    ShaderId RegisterManual(std::string spv_path, std::string slang_path,
                            ShaderStage stage);

    [[nodiscard]] std::expected<vk::ShaderModule, std::string> GetModule(ShaderId id);
    [[nodiscard]] const ShaderModuleSlot& GetSlot(ShaderId id) const;
    [[nodiscard]] ShaderId FindBySlangPath(std::string_view path) const;
    [[nodiscard]] std::uint64_t GetVersion(ShaderId id) const;

    std::future<bool> RequestReload(ShaderId id);
    std::future<bool> RequestReloadByPath(std::string_view slang_path);

    [[nodiscard]] vk::PipelineCache GetPipelineCache() const;
    [[nodiscard]] const vk::raii::PipelineCache& GetPipelineCacheRAII() const;

    void SerializeCache();
    void FlushCache();

private:
    bool LoadSpirvFromDisk(ShaderModuleSlot& slot);

    const vk::raii::Device& device_;
    vk::raii::PipelineCache cache_{nullptr};
    std::vector<std::unique_ptr<ShaderModuleSlot>> slots_;
    std::filesystem::path cache_path_;
    mutable std::shared_mutex slots_mutex_;
};

} // namespace VulkanEngine::ShaderSystem
