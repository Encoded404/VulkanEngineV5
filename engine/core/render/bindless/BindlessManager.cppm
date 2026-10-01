module;

export module VulkanEngine.BindlessManager;

import std;
import std.compat;

import vulkan_hpp;

export import VulkanBackend.Vulkan.VulkanBootstrap;
export import VulkanEngine.GpuResources;
import VulkanEngine.ResourceSystem;

export namespace VulkanEngine::BindlessManager {

// Slot 0 is permanently reserved as the fallback descriptor. Allocation
// starts at slot 1; a failed texture load binds the caller's slot index 0 so
// every material references the checkerboard through the same slot.
inline constexpr std::uint32_t kFallbackSlot = 0;

class BindlessManager {
public:
    BindlessManager() = default;
    ~BindlessManager();

    BindlessManager(const BindlessManager&) = delete;
    BindlessManager& operator=(const BindlessManager&) = delete;

    bool Initialize(VulkanBackend::Vulkan::IVulkanBootstrap& backend);
    void Shutdown();

    // Binds the permanent fallback descriptor at slot 0. Must be called before
    // the first AllocateTextureSlot (the fallback is always sampleable).
    void SetFallback(VulkanEngine::GpuResources::GpuTexture texture, const VulkanEngine::ResourceId& id);

    [[nodiscard]] std::uint32_t AllocateTextureSlot(VulkanEngine::GpuResources::GpuTexture texture, const VulkanEngine::ResourceId& id);
    [[nodiscard]] const VulkanEngine::ResourceId* GetTextureId(std::uint32_t slot) const;
    [[nodiscard]] const VulkanEngine::GpuResources::GpuTexture* GetTexture(std::uint32_t slot) const;

    [[nodiscard]] vk::DescriptorSetLayout* GetLayout();
    [[nodiscard]] vk::DescriptorSet GetDescriptorSet() const;
    [[nodiscard]] bool IsValid() const { return *descriptor_set_ != nullptr; }

private:
    void UpdateSlot(std::uint32_t slot, const VulkanEngine::GpuResources::GpuTexture& texture);

    VulkanBackend::Vulkan::IVulkanBootstrap* backend_ = nullptr;
    std::unique_ptr<vk::raii::DescriptorSetLayout> layout_{};
    std::unique_ptr<vk::raii::DescriptorPool> pool_{};
    vk::raii::DescriptorSet descriptor_set_{nullptr};
    std::vector<VulkanEngine::GpuResources::GpuTexture> textures_{}; // keep alive; slot 0 = fallback
    std::vector<VulkanEngine::ResourceId> texture_ids_{};
    std::uint32_t next_slot_ = kFallbackSlot;
    bool fallback_ready_ = false;
};

}
