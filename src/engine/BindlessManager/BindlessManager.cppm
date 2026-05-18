module;

#include <cstdint>
#include <memory>
#include <vector>
#include <vulkan/vulkan_raii.hpp>

#include <vulkan/vulkan_raii.hpp>

export module VulkanEngine.BindlessManager;

export import VulkanBackend.Runtime.VulkanBootstrap;
export import VulkanEngine.GpuResources;

export namespace VulkanEngine::BindlessManager {

class BindlessManager {
public:
    BindlessManager() = default;
    ~BindlessManager();

    BindlessManager(const BindlessManager&) = delete;
    BindlessManager& operator=(const BindlessManager&) = delete;

    bool Initialize(VulkanEngine::Runtime::IVulkanBootstrapBackend& backend);
    void Shutdown();

    [[nodiscard]] uint32_t AllocateTextureSlot(VulkanEngine::GpuResources::GpuTexture texture);

    [[nodiscard]] vk::DescriptorSetLayout* GetLayout();
    [[nodiscard]] vk::DescriptorSet GetDescriptorSet() const;
    [[nodiscard]] bool IsValid() const { return descriptor_set_ != nullptr; }

private:
    void UpdateSlot(uint32_t slot, const VulkanEngine::GpuResources::GpuTexture& texture);

    VulkanEngine::Runtime::IVulkanBootstrapBackend* backend_ = nullptr;
    std::unique_ptr<vk::raii::DescriptorSetLayout> layout_{};
    std::unique_ptr<vk::raii::DescriptorPool> pool_{};
    vk::raii::DescriptorSet descriptor_set_{nullptr};
    std::vector<VulkanEngine::GpuResources::GpuTexture> textures_{}; // keep alive
    uint32_t next_slot_ = 0;
};

}
