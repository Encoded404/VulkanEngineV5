module;

#include <cstdint>
#include <memory>

#include <vulkan/vulkan_raii.hpp>

export module VulkanEngine.GpuDescriptorSet;

export import VulkanEngine.Runtime.VulkanBootstrap;
export import VulkanEngine.GpuBuffer;
export import VulkanEngine.GpuTexture;

namespace VulkanEngine::GpuResources {
    export class GpuDescriptorSet;
}

export namespace VulkanEngine::GpuResources {

    struct DescriptorPoolConfig {
        uint32_t max_sets = 100;
        uint32_t max_combined_image_samplers = 100;
        uint32_t max_uniform_buffers = 10;
        uint32_t max_storage_buffers = 10;
        uint32_t max_storage_images = 10;
    };

    class DescriptorPool : public std::enable_shared_from_this<DescriptorPool> {
    public:
        static std::shared_ptr<DescriptorPool> Create(
            VulkanEngine::Runtime::IVulkanBootstrapBackend& backend,
            const DescriptorPoolConfig& config = {});

        ~DescriptorPool();

        DescriptorPool(const DescriptorPool&) = delete;
        DescriptorPool& operator=(const DescriptorPool&) = delete;
        DescriptorPool(DescriptorPool&&) = delete;
        DescriptorPool& operator=(DescriptorPool&&) = delete;

        GpuDescriptorSet Allocate(vk::DescriptorSetLayout layout);

        [[nodiscard]] vk::DescriptorSetLayout* GetLayout() { return descriptor_set_layout_ ? const_cast<vk::DescriptorSetLayout*>(&**descriptor_set_layout_) : nullptr; }
        [[nodiscard]] const vk::DescriptorSetLayout* GetLayout() const { return descriptor_set_layout_ ? &**descriptor_set_layout_ : nullptr; }
        [[nodiscard]] VulkanEngine::Runtime::IVulkanBootstrapBackend* GetBackend() { return backend_; }
        [[nodiscard]] const VulkanEngine::Runtime::IVulkanBootstrapBackend* GetBackend() const { return backend_; }

    private:
        friend class GpuDescriptorSet;

        DescriptorPool() = default;

        void Initialize(VulkanEngine::Runtime::IVulkanBootstrapBackend& backend, const DescriptorPoolConfig& config);
        void FreeDescriptorSet(vk::DescriptorSet set) const;

        VulkanEngine::Runtime::IVulkanBootstrapBackend* backend_ = nullptr;
        std::unique_ptr<vk::raii::DescriptorPool> pool_;
        std::unique_ptr<vk::raii::DescriptorSetLayout> descriptor_set_layout_;
        DescriptorPoolConfig config_{};
    };

    class GpuDescriptorSet {
    public:
        GpuDescriptorSet() = default;
        ~GpuDescriptorSet();

        GpuDescriptorSet(const GpuDescriptorSet&) = delete;
        GpuDescriptorSet& operator=(const GpuDescriptorSet&) = delete;
        GpuDescriptorSet(GpuDescriptorSet&& other) noexcept;
        GpuDescriptorSet& operator=(GpuDescriptorSet&& other) noexcept;

        void UpdateBinding(uint32_t binding,
                           const GpuTexture& texture,
                           vk::ImageLayout layout = vk::ImageLayout::eShaderReadOnlyOptimal) const;

        void UpdateBinding(uint32_t binding,
                           const GpuBuffer& buffer,
                           vk::DescriptorType type,
                           uint64_t size = 0,
                           uint64_t offset = 0) const;

        [[nodiscard]] vk::DescriptorSet GetHandle() const { return descriptor_set_; }
        [[nodiscard]] bool IsValid() const { return descriptor_set_ != nullptr; }

    private:
        friend class DescriptorPool;

        static GpuDescriptorSet Create(
            VulkanEngine::Runtime::IVulkanBootstrapBackend* backend,
            std::shared_ptr<DescriptorPool> pool,
            vk::DescriptorSet set,
            vk::DescriptorSetLayout layout);

        GpuDescriptorSet(
            VulkanEngine::Runtime::IVulkanBootstrapBackend* backend,
            std::shared_ptr<DescriptorPool> pool,
            vk::DescriptorSet set,
            vk::DescriptorSetLayout layout);

        void Destroy();

        VulkanEngine::Runtime::IVulkanBootstrapBackend* backend_ = nullptr;
        std::shared_ptr<DescriptorPool> pool_;
        vk::DescriptorSet descriptor_set_ = nullptr;
        vk::DescriptorSetLayout layout_ = nullptr;
        bool destroyed_ = false;
    };

} // namespace VulkanEngine::GpuResources
