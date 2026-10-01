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
import VulkanEngine.GpuResources.FrameRing;

namespace VulkanEngine::BindlessManager {

BindlessManager::~BindlessManager() {
    Shutdown();
}

bool BindlessManager::Initialize(VulkanBackend::Vulkan::IVulkanBootstrap& backend,
                                 const BindlessCapacityConfig& capacity_config) {
    backend_ = &backend;
    const auto& device = backend.GetDevice();

    // One explicit capacity, clamped by the device limits minus the other
    // update-after-bind pools' descriptor counts, then used for the layout,
    // pool and the fixed descriptor count. Never size from the raw device
    // limits: the pool descriptorCount is a ceiling the driver may back eagerly
    // and it counts against the global update-after-bind budget.
    const auto& desc_caps = backend.GetCapabilities().GetDescriptorCapabilities();
    BindlessCapacityLimits limits{};
    limits.max_combined_image_samplers = desc_caps.max_bindless_combined_image_samplers;
    limits.max_update_after_bind_in_all_pools =
        desc_caps.max_update_after_bind_descriptors_in_all_pools;
    capacity_ = ComputeBindlessCapacity(capacity_config, limits);
    capacity_ = std::min(capacity_, desc_caps.max_bindless_combined_image_samplers);

    LOGIFACE_LOG(debug, "BindlessManager: bindless_capacity=" + std::to_string(capacity_));

    // Fixed descriptor count (no VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT):
    // the capacity is known up front and the set is always allocated at it.
    vk::DescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = vk::DescriptorType::eCombinedImageSampler;
    binding.descriptorCount = capacity_;
    binding.stageFlags = vk::ShaderStageFlagBits::eFragment;
    binding.pImmutableSamplers = nullptr;

    constexpr vk::DescriptorBindingFlags binding_flags =
        vk::DescriptorBindingFlagBits::ePartiallyBound |
        vk::DescriptorBindingFlagBits::eUpdateAfterBind;

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

    vk::DescriptorPoolSize pool_size{};
    pool_size.type = vk::DescriptorType::eCombinedImageSampler;
    pool_size.descriptorCount = capacity_;

    vk::DescriptorPoolCreateInfo pool_info{};
    pool_info.flags = vk::DescriptorPoolCreateFlagBits::eUpdateAfterBind
                    | vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;

    pool_ = std::make_unique<vk::raii::DescriptorPool>(device, pool_info);
    VulkanBackend::Vulkan::SetVulkanObjectName(device, *pool_, "bindless-pool");

    vk::DescriptorSetAllocateInfo alloc_info{};
    alloc_info.descriptorPool = **pool_;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &**layout_;

    auto sets = device.allocateDescriptorSets(alloc_info);
    if (sets.empty()) {
        LOGIFACE_LOG(error, "BindlessManager: failed to allocate descriptor set");
        return false;
    }
    descriptor_set_ = std::move(sets[0]);
    VulkanBackend::Vulkan::SetVulkanObjectName(device, descriptor_set_, "bindless-descriptor-set");

    slots_.resize(capacity_);
    free_slots_.clear();
    free_slots_.reserve(capacity_ - 1U);
    // Allocation starts at slot 1 (slot 0 is the permanent fallback); the free
    // list is LIFO so the lowest slots are handed out only after they recycle.
    for (std::uint32_t slot = capacity_; slot-- > kFallbackSlot + 1U;) {
        free_slots_.push_back(slot);
    }

    ring_.Initialize(backend.GetFramesInFlight());

    LOGIFACE_LOG(debug, "BindlessManager initialized; slot 0 reserved for the fallback");
    return true;
}

void BindlessManager::Shutdown() {
    // Destroy descriptor set before pool (it references the pool).
    descriptor_set_ = vk::raii::DescriptorSet{nullptr};
    // Release every live binding: GpuTexture destructors free heap records; the
    // image heap outlives this manager per the EngineBootstrap teardown order.
    for (auto& slot : slots_) {
        slot.binding = VulkanEngine::GpuResources::GpuTexture{};
    }
    slots_.clear();
    free_slots_.clear();
    ring_ = VulkanEngine::GpuResources::FrameRing<SlotOp>{};
    pool_.reset();
    layout_.reset();
    backend_ = nullptr;
    capacity_ = 0;
    next_generation_ = 1;
    fallback_ready_ = false;
}

void BindlessManager::SetFallback(VulkanEngine::GpuResources::GpuTexture texture,
                                  const VulkanEngine::ResourceId& id) {
    if (!backend_ || fallback_ready_ || slots_.empty()) {
        return;
    }
    auto& slot = slots_[kFallbackSlot];
    slot.id = id;
    slot.binding = std::move(texture);
    slot.generation = next_generation_++;
    slot.users = 1;      // the fallback never reaches zero users
    slot.released = false;
    slot.committed = true;
    UpdateSlot(kFallbackSlot, slot.binding);
    fallback_ready_ = true;
    LOGIFACE_LOG(debug, "BindlessManager: fallback texture bound at slot " +
                            std::to_string(kFallbackSlot) + " ('" + id.value + "')");
}

std::optional<TextureHandle> BindlessManager::ReserveSlot(const VulkanEngine::ResourceId& id) {
    if (!fallback_ready_ || free_slots_.empty()) {
        return std::nullopt;
    }
    const std::uint32_t slot_index = free_slots_.back();
    free_slots_.pop_back();
    auto& slot = slots_[slot_index];
    slot.generation = next_generation_++;
    slot.users = 0;
    slot.released = false;
    slot.committed = false;
    slot.id = id;
    slot.binding = VulkanEngine::GpuResources::GpuTexture{};
    // Pre-bind the fallback so a material can sample immediately; never leave
    // the descriptor undefined.
    WriteFallback(slot_index);
    return TextureHandle{.slot = slot_index, .generation = slot.generation};
}

void BindlessManager::CommitSlot(TextureHandle handle, VulkanEngine::GpuResources::GpuTexture binding,
                                 std::uint32_t recording_frame) {
    if (handle.slot >= slots_.size()) {
        return;
    }
    auto& slot = slots_[handle.slot];
    if (slot.generation != handle.generation || slot.released) {
        return;  // stale reservation; drop
    }
    slot.users = std::max(slot.users, 1U);
    ring_.Enqueue(SlotOp{
        .kind = OpKind::Publish,
        .slot = handle.slot,
        .generation = handle.generation,
        .recording_frame = recording_frame,
        .binding = std::move(binding),
    }, recording_frame, /*gated=*/true);
}

void BindlessManager::ReleaseSlot(TextureHandle handle, std::uint32_t recording_frame) {
    if (handle.slot >= slots_.size()) {
        return;
    }
    auto& slot = slots_[handle.slot];
    if (slot.generation != handle.generation || slot.released) {
        return;
    }
    if (slot.users > 0) {
        --slot.users;
    }
    if (slot.users == 0) {
        slot.released = true;
        ring_.Enqueue(SlotOp{
            .kind = OpKind::Decommit,
            .slot = handle.slot,
            .generation = handle.generation,
            .recording_frame = recording_frame,
        }, recording_frame, /*gated=*/false);
    }
}

std::optional<TextureHandle> BindlessManager::AllocateTextureSlot(
    VulkanEngine::GpuResources::GpuTexture texture, const VulkanEngine::ResourceId& id) {
    if (!fallback_ready_) {
        LOGIFACE_LOG(error, "BindlessManager: AllocateTextureSlot before SetFallback is a bug; texture '"
                                + id.value + "' rejected");
        return std::nullopt;
    }
    if (free_slots_.empty()) {
        LOGIFACE_LOG(warn, "BindlessManager: bindless capacity exhausted (" + std::to_string(capacity_) +
                               "); texture '" + id.value + "' rejected");
        return std::nullopt;
    }
    const std::uint32_t slot_index = free_slots_.back();
    free_slots_.pop_back();
    auto& slot = slots_[slot_index];
    slot.generation = next_generation_++;
    slot.users = 1;
    slot.released = false;
    slot.committed = true;
    slot.id = id;
    slot.binding = std::move(texture);
    UpdateSlot(slot_index, slot.binding);
    const TextureHandle handle{.slot = slot_index, .generation = slot.generation};
    LOGIFACE_LOG(debug, "Allocated bindless texture slot " + std::to_string(slot_index) + " for texture '" + id.value + "'");
    return handle;
}

void BindlessManager::ReleaseSlotImmediate(std::uint32_t slot_index) {
    if (slot_index <= kFallbackSlot || slot_index >= slots_.size()) {
        return;
    }
    auto& slot = slots_[slot_index];
    WriteFallback(slot_index);
    slot.binding = VulkanEngine::GpuResources::GpuTexture{};
    slot.id = {};
    slot.users = 0;
    slot.committed = false;
    slot.released = false;
    slot.generation = next_generation_++;
    free_slots_.push_back(slot_index);
}

void BindlessManager::BeginFrame(std::uint32_t frame_index,
                                 const std::function<bool(std::uint32_t)>& gate,
                                 const std::function<void(TextureHandle, std::uint32_t)>& on_drop) {
    ring_.BeginFrame(
        frame_index,
        [this](SlotOp& op, std::uint32_t) { ApplyOp(op); },
        [&gate](std::uint32_t recording_frame) {
            // A gated op applies only when its recording frame was submitted.
            return gate && gate(recording_frame);
        },
        [&on_drop](SlotOp& op, std::uint32_t recording_frame) {
            if (op.kind == OpKind::Publish && on_drop) {
                on_drop(TextureHandle{.slot = op.slot, .generation = op.generation}, recording_frame);
            }
            // Decommit/Destroy are never gated, so they do not reach here.
        });
}

void BindlessManager::ApplyOp(SlotOp& op) {
    if (op.slot >= slots_.size()) {
        return;
    }
    auto& slot = slots_[op.slot];
    if (slot.generation != op.generation) {
        return;  // slot recycled since the op was queued
    }
    switch (op.kind) {
        case OpKind::Publish: {
            if (slot.released) {
                // The reservation was released before the upload landed.
                DestroyBinding(op.binding);
                return;
            }
            // Move any prior binding into the graveyard; the fallback (slot 0)
            // is never replaced through this path.
            VulkanEngine::GpuResources::GpuTexture retired = std::move(slot.binding);
            slot.binding = std::move(op.binding);
            slot.committed = true;
            UpdateSlot(op.slot, slot.binding);
            if (retired.IsValid() && retired.GetImage() != nullptr) {
                // Destroy one ring cycle later, once every frame that could have
                // bound the old descriptor is complete.
                ring_.Enqueue(SlotOp{
                    .kind = OpKind::Destroy,
                    .slot = op.slot,
                    .generation = op.generation,
                    .recording_frame = op.recording_frame,
                    .binding = std::move(retired),
                }, op.recording_frame, /*gated=*/false);
            }
            return;
        }
        case OpKind::Decommit: {
            if (slot.released && slot.users == 0) {
                WriteFallback(op.slot);
                VulkanEngine::GpuResources::GpuTexture retired = std::move(slot.binding);
                slot.binding = VulkanEngine::GpuResources::GpuTexture{};
                slot.committed = false;
                if (retired.IsValid() && retired.GetImage() != nullptr) {
                    ring_.Enqueue(SlotOp{
                        .kind = OpKind::Destroy,
                        .slot = op.slot,
                        .generation = op.generation,
                        .recording_frame = op.recording_frame,
                        .binding = std::move(retired),
                    }, op.recording_frame, /*gated=*/false);
                }
                // Return the slot to the free list only now, so a queued Publish
                // for the old reservation can never land on a re-reserved slot.
                slot.released = false;
                slot.generation = next_generation_++;
                free_slots_.push_back(op.slot);
            }
            return;
        }
        case OpKind::Destroy: {
            DestroyBinding(op.binding);
            return;
        }
    }
}

void BindlessManager::DestroyBinding(VulkanEngine::GpuResources::GpuTexture& binding) {
    binding = VulkanEngine::GpuResources::GpuTexture{};
}

const VulkanEngine::ResourceId* BindlessManager::GetTextureId(std::uint32_t slot) const {
    if (slot < slots_.size() && slots_[slot].id.value.size() > 0) {
        return &slots_[slot].id;
    }
    return nullptr;
}

const VulkanEngine::GpuResources::GpuTexture* BindlessManager::GetTexture(std::uint32_t slot) const {
    if (slot < slots_.size()) return &slots_[slot].binding;
    return nullptr;
}

const VulkanEngine::GpuResources::GpuTexture* BindlessManager::GetTexture(TextureHandle handle) const {
    if (handle.slot >= slots_.size() || slots_[handle.slot].generation != handle.generation) {
        return nullptr;
    }
    // A reserved-but-uncommitted slot's descriptor points at the fallback; the
    // slot record holds no own binding until the publish applies, so report the
    // fallback as the effective texture (this is what a pending material samples).
    const auto& slot = slots_[handle.slot];
    if (!slot.committed || !slot.binding.IsValid()) {
        return &slots_[kFallbackSlot].binding;
    }
    return &slot.binding;
}

void BindlessManager::WriteFallback(std::uint32_t slot) {
    if (!fallback_ready_ && slot != kFallbackSlot) {
        return;
    }
    // Point the slot at slot 0's live fallback view/sampler.
    if (slots_.empty()) return;
    const auto& fallback = slots_[kFallbackSlot].binding;
    if (!fallback.IsValid() || fallback.GetImageView() == nullptr) {
        return;
    }
    if (slot == kFallbackSlot) {
        UpdateSlot(slot, fallback);
        return;
    }
    if (!backend_ || *descriptor_set_ == nullptr) return;
    vk::DescriptorImageInfo image_info{};
    image_info.sampler = fallback.GetSampler();
    image_info.imageView = fallback.GetImageView();
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

void BindlessManager::UpdateSlot(std::uint32_t slot, const VulkanEngine::GpuResources::GpuTexture& texture) {
    if (!backend_ || *descriptor_set_ == nullptr) return;
    if (slot >= capacity_) return;
    if (!texture.IsValid() || texture.GetImageView() == nullptr) return;

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

} // namespace VulkanEngine::BindlessManager
