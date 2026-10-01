module;

#include <logging/logging_macros.hpp>

module VulkanEngine.BindlessManager;

import std;
import std.compat;

import logiface;

import vulkan_hpp;

import VulkanBackend.Vulkan.VulkanBootstrap;
import VulkanBackend.Vulkan.VulkanDebugUtils;
import VulkanEngine.GpuResources;

namespace VulkanEngine::BindlessManager {

BindlessManager::~BindlessManager() {
    Shutdown();
}

bool BindlessManager::Initialize(VulkanBackend::Vulkan::IVulkanBootstrap& backend) {
    backend_ = &backend;
    const auto& device = backend.GetDevice();

    // Query device limits for update-after-bind descriptors (from the capabilities snapshot)
    const auto& indexing_props = backend.GetCapabilities().GetDescriptorIndexingProperties();
    const std::uint32_t max_samplers = indexing_props.maxDescriptorSetUpdateAfterBindSampledImages;

    LOGIFACE_LOG(debug, "maxDescriptorSetUpdateAfterBindSamplers=" + std::to_string(max_samplers));

    // The bindless array lives in a pipeline layout together with app sets, and
    // the device limits count sampled images across every set in the layout.
    // Reserve a little headroom so app passes can declare their own
    // combined-image-sampler bindings without pushing the layout over.
    constexpr std::uint32_t kAppSampledReserve = 64;
    const std::uint32_t layout_count =
        max_samplers > kAppSampledReserve ? max_samplers - kAppSampledReserve : max_samplers;

    // Create descriptor set layout with maximum allowed sampler array size
    vk::DescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = vk::DescriptorType::eCombinedImageSampler;
    binding.descriptorCount = layout_count;
    binding.stageFlags = vk::ShaderStageFlagBits::eFragment;
    binding.pImmutableSamplers = nullptr;

    constexpr vk::DescriptorBindingFlags binding_flags =
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
    VulkanBackend::Vulkan::SetVulkanObjectName(device, *layout_, "bindless-layout");

    // Create descriptor pool — reasonably large, can grow if needed
    const std::uint32_t MAX_POOL_TEXTURES = std::min<std::uint32_t>(65536u, layout_count);
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
    VulkanBackend::Vulkan::SetVulkanObjectName(device, *pool_, "bindless-pool");

    // Allocate with variable descriptor count — start with capacity for 1024
    const std::uint32_t variable_count = std::min<std::uint32_t>(1024u, layout_count);
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
    VulkanBackend::Vulkan::SetVulkanObjectName(device, descriptor_set_, "bindless-descriptor-set");

    LOGIFACE_LOG(debug, "BindlessManager initialized; slot 0 reserved for the fallback");
    return true;
}

void BindlessManager::Shutdown() {
    // Destroy descriptor set before pool (it references the pool)
    descriptor_set_ = vk::raii::DescriptorSet{nullptr};
    // Release the textures (their GpuTexture destructors free heap records;
    // the image heap outlives this manager per the EngineBootstrap teardown
    // ordering).
    textures_.clear();
    texture_ids_.clear();
    pool_.reset();
    layout_.reset();
    backend_ = nullptr;
    next_slot_ = kFallbackSlot;
    fallback_ready_ = false;
}

// Slot 0 is the permanent fallback. Its texture stays alive for the
// manager's lifetime (never released, never reused) and its descriptor is
// written before any allocation, so a slot reserved by a later commit can
// point here while its real image is still uploading.
void BindlessManager::SetFallback(VulkanEngine::GpuResources::GpuTexture texture,
                                  const VulkanEngine::ResourceId& id) {
    if (!backend_ || fallback_ready_) {
        return;
    }
    textures_.resize(kFallbackSlot + 1);
    texture_ids_.resize(kFallbackSlot + 1);
    textures_[kFallbackSlot] = std::move(texture);
    texture_ids_[kFallbackSlot] = id;
    UpdateSlot(kFallbackSlot, textures_[kFallbackSlot]);
    fallback_ready_ = true;
    LOGIFACE_LOG(debug, "BindlessManager: fallback texture bound at slot " +
                            std::to_string(kFallbackSlot) + " ('" + id.value + "')");
}

uint32_t BindlessManager::AllocateTextureSlot(VulkanEngine::GpuResources::GpuTexture texture, const VulkanEngine::ResourceId& id) {
    if (!fallback_ready_) {
        LOGIFACE_LOG(error, "BindlessManager: AllocateTextureSlot before SetFallback is a bug; texture '"
                                + id.value + "' rejected");
        return kFallbackSlot;
    }
    // Allocation starts at 1; slot 0 is the permanently-bound fallback.
    const std::uint32_t slot = std::max<std::uint32_t>(next_slot_, kFallbackSlot + 1U);
    next_slot_ = slot + 1U;
    if (slot >= textures_.size()) {
        textures_.resize(slot + 1);
        texture_ids_.resize(slot + 1);
    }
    textures_[slot] = std::move(texture);
    texture_ids_[slot] = id;
    UpdateSlot(slot, textures_[slot]);
    LOGIFACE_LOG(debug, "Allocated bindless texture slot " + std::to_string(slot) + " for texture '" + id.value + "'");
    return slot;
}

const VulkanEngine::ResourceId* BindlessManager::GetTextureId(std::uint32_t slot) const {
    if (slot < texture_ids_.size()) return &texture_ids_[slot];
    return nullptr;
}

const VulkanEngine::GpuResources::GpuTexture* BindlessManager::GetTexture(std::uint32_t slot) const {
    if (slot < textures_.size()) return &textures_[slot];
    return nullptr;
}

void BindlessManager::UpdateSlot(std::uint32_t slot, const VulkanEngine::GpuResources::GpuTexture& texture) {
    if (!backend_ || *descriptor_set_ == nullptr) return;

    vk::DescriptorImageInfo image_info{};
    image_info.sampler = texture.GetSampler();
    image_info.imageView = texture.GetImageView();
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
