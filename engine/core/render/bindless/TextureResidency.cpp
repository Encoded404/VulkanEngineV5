module;

#include <logging/logging_macros.hpp>

module VulkanEngine.TextureResidency;

import std;
import std.compat;

import logiface;

import VulkanEngine.BindlessManager;
import VulkanEngine.ResidencyPolicy;
import VulkanEngine.MaterialManager;

namespace VulkanEngine::Textures {

void TextureResidency::Initialize(BindlessManager::BindlessManager& bindless,
                                  MaterialManager::MaterialManager& materials,
                                  const VulkanBackend::Vulkan::VulkanCapabilities& capabilities,
                                  std::uint64_t fallback_budget_bytes) {
    bindless_ = &bindless;
    materials_ = &materials;
    capabilities_ = &capabilities;
    budget_override_ = fallback_budget_bytes;
    extension_budget_ = 0;

    // With VK_EXT_memory_budget, derive a soft budget from the first heap that
    // reports one (usually DEVICE_LOCAL). The engine only uses it as an
    // eviction high-water mark; it never allocates against it. Without the
    // extension this stays 0 and the caller's override (or "disabled") wins.
    if (capabilities.HasMemoryBudget()) {
        for (const auto& heap : capabilities.GetMemoryBudgetSnapshot()) {
            if (heap.budget != 0) {
                extension_budget_ = heap.budget;
                break;
            }
        }
    }
    LOGIFACE_LOG(debug, std::format("TextureResidency: budget={} bytes, extension_budget={}",
                                    EffectiveBudget(), extension_budget_));
}

void TextureResidency::Shutdown() {
    set_ = Residency::ResidencySet{};
    key_to_id_.clear();
    id_to_handle_.clear();
    bindless_ = nullptr;
    materials_ = nullptr;
    capabilities_ = nullptr;
    budget_override_ = 0;
    extension_budget_ = 0;
    next_id_ = 1;
}

std::uint64_t TextureResidency::EffectiveBudget() const {
    if (budget_override_ != 0) {
        return budget_override_;
    }
    return extension_budget_;
}

void TextureResidency::TrackReservation(BindlessManager::TextureHandle handle,
                                        std::uint64_t bytes, const ResourceId& id,
                                        std::uint64_t frame, bool pinned) {
    (void)id;
    if (!handle.IsValid()) {
        return;
    }
    const Key key{handle.slot, handle.generation};
    auto it = key_to_id_.find(key);
    std::uint64_t residency_id = 0;
    if (it != key_to_id_.end()) {
        residency_id = it->second;
    } else {
        residency_id = next_id_++;
        key_to_id_[key] = residency_id;
        id_to_handle_[residency_id] = handle;
    }
    set_.Track(residency_id, bytes, frame, pinned);
}

void TextureResidency::MarkResident(BindlessManager::TextureHandle handle, std::uint64_t bytes,
                                    std::uint64_t frame) {
    const auto it = key_to_id_.find(Key{handle.slot, handle.generation});
    if (it == key_to_id_.end()) {
        return;
    }
    set_.MarkResident(it->second, bytes, frame);
}

void TextureResidency::Touch(BindlessManager::TextureHandle handle, std::uint64_t frame) {
    const auto it = key_to_id_.find(Key{handle.slot, handle.generation});
    if (it == key_to_id_.end()) {
        return;
    }
    set_.Touch(it->second, frame);
}

void TextureResidency::Forget(BindlessManager::TextureHandle handle) {
    const auto it = key_to_id_.find(Key{handle.slot, handle.generation});
    if (it == key_to_id_.end()) {
        return;
    }
    set_.MarkEvicted(it->second);
    id_to_handle_.erase(it->second);
    key_to_id_.erase(it);
}

void TextureResidency::RegisterResourceHandle(const ResourceId& id,
                                              BindlessManager::TextureHandle handle,
                                              std::uint64_t frame) {
    resource_handles_[id.value] = handle;
    const Key key{handle.slot, handle.generation};
    if (!key_to_id_.contains(key)) {
        TrackReservation(handle, 0, id, frame);
    }
}

std::optional<BindlessManager::TextureHandle> TextureResidency::HandleForResource(
    const ResourceId& id) const {
    const auto it = resource_handles_.find(id.value);
    if (it == resource_handles_.end()) {
        return std::nullopt;
    }
    return it->second;
}

void TextureResidency::ForgetResource(const ResourceId& id) {
    const auto it = resource_handles_.find(id.value);
    if (it != resource_handles_.end()) {
        Forget(it->second);
        resource_handles_.erase(it);
    }
}

std::vector<std::pair<ResourceId, BindlessManager::TextureHandle>>
TextureResidency::RegisteredResources() const {
    std::vector<std::pair<ResourceId, BindlessManager::TextureHandle>> out;
    out.reserve(resource_handles_.size());
    for (const auto& [key, handle] : resource_handles_) {
        out.emplace_back(ResourceId{key}, handle);
    }
    return out;
}

std::uint32_t TextureResidency::Collect(std::uint32_t frame,
                                        std::uint64_t min_residency_frames) {
    const std::uint64_t budget = EffectiveBudget();
    if (budget == 0 || bindless_ == nullptr || materials_ == nullptr) {
        return 0;
    }
    // If a texture is referenced by a live material it is in active use and is
    // not the eviction candidate, so refresh its age from the material index.
    // Rebuild the index only when eviction is actually possible (over budget);
    // a set that fits costs nothing per frame.
    if (set_.ResidentBytes() > budget) {
        materials_->RebuildTextureSlotIndex();
        for (const auto& [resource_key, handle] : resource_handles_) {
            (void)resource_key;
            bool referenced = false;
            materials_->ForEachMaterialUsingSlot(
                handle.slot, [&referenced](MaterialManager::MaterialId) { referenced = true; });
            if (referenced) {
                Touch(handle, frame);
            }
        }
    }

    const Residency::EvictionPlan plan = set_.Plan(budget, frame, min_residency_frames);
    if (plan.evict.empty()) {
        return 0;
    }

    std::uint32_t evicted = 0;
    for (const std::uint64_t residency_id : plan.evict) {
        const auto handle_it = id_to_handle_.find(residency_id);
        if (handle_it == id_to_handle_.end()) {
            continue;
        }
        const BindlessManager::TextureHandle handle = handle_it->second;
        // Release the bindless slot: the descriptor is reset to the fallback at
        // the ring drain and the slot is returned to the free list there. The
        // materials referencing it move to the fallback immediately (their CPU
        // payload), so no material samples a decommitted slot.
        materials_->RewriteTextureSlot(handle.slot, BindlessManager::kFallbackSlot);
        bindless_->ReleaseSlot(handle, frame);

        set_.MarkEvicted(residency_id);
        key_to_id_.erase(Key{handle.slot, handle.generation});
        id_to_handle_.erase(handle_it);
        ++evicted;
        LOGIFACE_LOG(debug, std::format("TextureResidency: evicted slot {} ({} bytes)",
                                        handle.slot, plan.evicted_bytes));
    }
    return evicted;
}

} // namespace VulkanEngine::Textures
