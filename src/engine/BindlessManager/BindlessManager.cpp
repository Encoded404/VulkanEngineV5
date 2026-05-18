module;

#include <cstdint>
#include <vector>
#include <array>

#include <vulkan/vulkan_raii.hpp>

#include <logging/logging.hpp>

module VulkanEngine.BindlessManager;

import VulkanBackend.Runtime.VulkanBootstrap;
import VulkanEngine.GpuResources;

namespace VulkanEngine::BindlessManager {

BindlessManager::~BindlessManager() {
    Shutdown();
}

bool BindlessManager::Initialize(VulkanEngine::Runtime::IVulkanBootstrapBackend& backend) {
    backend_ = &backend;
    const auto& device = backend.GetDevice();

    // Query device limits for update-after-bind descriptors
    auto props = backend.GetPhysicalDevice().getProperties2<
        vk::PhysicalDeviceProperties2,
        vk::PhysicalDeviceDescriptorIndexingProperties>();
    const auto& indexing_props = props.get<vk::PhysicalDeviceDescriptorIndexingProperties>();
    const uint32_t max_samplers = indexing_props.maxDescriptorSetUpdateAfterBindSamplers;

    LOGIFACE_LOG(debug, "maxDescriptorSetUpdateAfterBindSamplers=" + std::to_string(max_samplers));

    // Create descriptor set layout with maximum allowed sampler array size
    vk::DescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = vk::DescriptorType::eCombinedImageSampler;
    binding.descriptorCount = max_samplers;
    binding.stageFlags = vk::ShaderStageFlagBits::eFragment;
    binding.pImmutableSamplers = nullptr;

    vk::DescriptorBindingFlags binding_flags =
        vk::DescriptorBindingFlagBits::ePartiallyBound |
        vk::DescriptorBindingFlagBits::eUpdateAfterBind |
        vk::DescriptorBindingFlagBits::eVariableDescriptorCount;

    vk::DescriptorSetLayoutBindingFlagsCreateInfo binding_flags_info{};
    binding_flags_info.bindingCount = 1;
    binding_flags_info.pBindingFlags = &binding_flags;

    vk::DescriptorSetLayoutCreateInfo layout_info{};
    layout_info.flags = vk::DescriptorSetLayoutCreateFlagBits::eUpdateAfterBindPool;
    layout_info.pNext = &binding_flags_info;
    layout_info.bindingCount = 1;
    layout_info.pBindings = &binding;

    layout_ = std::make_unique<vk::raii::DescriptorSetLayout>(device, layout_info);

    // Create descriptor pool — reasonably large, can grow if needed
    constexpr uint32_t MAX_POOL_TEXTURES = 65536;
    vk::DescriptorPoolSize pool_size{};
    pool_size.type = vk::DescriptorType::eCombinedImageSampler;
    pool_size.descriptorCount = MAX_POOL_TEXTURES;

    vk::DescriptorPoolCreateInfo pool_info{};
    pool_info.flags = vk::DescriptorPoolCreateFlagBits::eUpdateAfterBind
                    | vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;

    pool_ = std::make_unique<vk::raii::DescriptorPool>(device, pool_info);

    // Allocate with variable descriptor count — start with capacity for 1024
    const uint32_t variable_count = 1024;
    vk::DescriptorSetVariableDescriptorCountAllocateInfo variable_count_info{};
    variable_count_info.descriptorSetCount = 1;
    variable_count_info.pDescriptorCounts = &variable_count;

    vk::DescriptorSetAllocateInfo alloc_info{};
    alloc_info.pNext = &variable_count_info;
    alloc_info.descriptorPool = **pool_;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &**layout_;

    auto sets = device.allocateDescriptorSets(alloc_info);
    if (sets.empty()) {
        LOGIFACE_LOG(error, "BindlessManager: failed to allocate descriptor set");
        return false;
    }
    // Move the RAII wrapper out of the vector to keep it alive
    descriptor_set_ = std::move(sets[0]);

    LOGIFACE_LOG(info, "BindlessManager initialized with unbounded texture array");
    return true;
}

void BindlessManager::Shutdown() {
    // Destroy descriptor set before pool (it references the pool)
    descriptor_set_ = vk::raii::DescriptorSet{nullptr};
    pool_.reset();
    layout_.reset();
    backend_ = nullptr;
    next_slot_ = 0;
}

uint32_t BindlessManager::AllocateTextureSlot(VulkanEngine::GpuResources::GpuTexture texture) {
    const uint32_t slot = next_slot_++;
    if (slot >= textures_.size()) {
        textures_.resize(slot + 1);
    }
    textures_[slot] = std::move(texture);
    UpdateSlot(slot, textures_[slot]);
    return slot;
}

void BindlessManager::UpdateSlot(uint32_t slot, const VulkanEngine::GpuResources::GpuTexture& texture) {
    if (!backend_ || descriptor_set_ == nullptr) return;

    vk::DescriptorImageInfo image_info{};
    image_info.sampler = *texture.GetSampler();
    image_info.imageView = *texture.GetImageView();
    image_info.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

    vk::WriteDescriptorSet write{};
    write.dstSet = *descriptor_set_;
    write.dstBinding = 0;
    write.dstArrayElement = slot;
    write.descriptorCount = 1;
    write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
    write.pImageInfo = &image_info;

    backend_->GetDevice().updateDescriptorSets({write}, {});
}

vk::DescriptorSetLayout* BindlessManager::GetLayout() {
    return layout_ ? const_cast<vk::DescriptorSetLayout*>(&**layout_) : nullptr;
}

vk::DescriptorSet BindlessManager::GetDescriptorSet() const {
    return *descriptor_set_;
}

}
