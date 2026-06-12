module;

export module VulkanEngine.BindlessManager;

import std;
import std.compat;

import vulkan_hpp;

export import VulkanBackend.Runtime.VulkanBootstrap;
export import VulkanEngine.GpuResources;
import VulkanEngine.ResourceSystem;

export namespace VulkanEngine::BindlessManager {

class BindlessManager {
public:
    BindlessManager() = default;
    ~BindlessManager();

    BindlessManager(const BindlessManager&) = delete;
    BindlessManager& operator=(const BindlessManager&) = delete;

    bool Initialize(VulkanEngine::Runtime::IVulkanBootstrap& backend);
    void Shutdown();

    [[nodiscard]] std::uint32_t AllocateTextureSlot(VulkanEngine::GpuResources::GpuTexture texture, const VulkanEngine::ResourceId& id);
    [[nodiscard]] const VulkanEngine::ResourceId* GetTextureId(std::uint32_t slot) const;

    [[nodiscard]] vk::DescriptorSetLayout* GetLayout();
    [[nodiscard]] vk::DescriptorSet GetDescriptorSet() const;
    [[nodiscard]] bool IsValid() const { return *descriptor_set_ != nullptr; }

private:
    void UpdateSlot(std::uint32_t slot, const VulkanEngine::GpuResources::GpuTexture& texture);

    VulkanEngine::Runtime::IVulkanBootstrap* backend_ = nullptr;
    std::unique_ptr<vk::raii::DescriptorSetLayout> layout_{};
    std::unique_ptr<vk::raii::DescriptorPool> pool_{};
    vk::raii::DescriptorSet descriptor_set_{nullptr};
    std::vector<VulkanEngine::GpuResources::GpuTexture> textures_{}; // keep alive
    std::vector<VulkanEngine::ResourceId> texture_ids_{};
    std::uint32_t next_slot_ = 0;
};

}
