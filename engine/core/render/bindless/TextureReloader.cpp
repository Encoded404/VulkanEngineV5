module;

#include <logging/logging_macros.hpp>

module VulkanEngine.TextureReloader;

import std;
import std.compat;

import logiface;

import VulkanEngine.ResourceSystem;
import VulkanEngine.ResourceSystem.TextureResource;
import VulkanEngine.BindlessManager;
import VulkanEngine.BindlessManager.TextureSlot;
import VulkanEngine.TextureUploader;
import VulkanEngine.TextureResidency;
import VulkanEngine.TextureWatcher;
import VulkanEngine.MaterialManager;
import VulkanEngine.TextureTypes;

namespace VulkanEngine::Textures {

void TextureReloader::Initialize(TextureSystem::TextureWatcher& watcher,
                                 ResourceManager& resources,
                                 BindlessManager::BindlessManager& bindless,
                                 TextureUploader& uploader,
                                 TextureResidency& residency,
                                 MaterialManager::MaterialManager& materials) {
    watcher_ = &watcher;
    resources_ = &resources;
    bindless_ = &bindless;
    uploader_ = &uploader;
    residency_ = &residency;
    materials_ = &materials;
}

void TextureReloader::Shutdown() {
    watched_.clear();
    pending_swaps_.clear();
    watcher_ = nullptr;
    resources_ = nullptr;
    bindless_ = nullptr;
    uploader_ = nullptr;
    residency_ = nullptr;
    materials_ = nullptr;
}

void TextureReloader::WatchResource(const ResourceId& id, const std::filesystem::path& path,
                                    TextureSemantic semantic, TextureNormalEncoding normal_encoding,
                                    const SamplerDesc& sampler) {
    if (watcher_ == nullptr || path.empty()) {
        return;
    }
    Watched watch{};
    watch.path = path;
    watch.semantic = semantic;
    watch.normal_encoding = normal_encoding;
    watch.sampler = sampler;
    watched_[id.value] = std::move(watch);
    (void)watcher_->AddDirectory(path.parent_path().string());
}

void TextureReloader::UnwatchResource(const ResourceId& id) {
    watched_.erase(id.value);
}

std::uint32_t TextureReloader::Pump(std::uint32_t frame_index) {
    if (watcher_ == nullptr || resources_ == nullptr) {
        return 0;
    }

    // 1. Apply swaps whose new binding has published. The old slot is only
    //    released now, after the new image is sampleable, so a frame that
    //    recorded the old binding has finished with it.
    std::uint32_t swapped = 0;
    for (auto it = pending_swaps_.begin(); it != pending_swaps_.end();) {
        if (!bindless_->IsCommitted(it->new_handle.slot)) {
            ++it;
            continue;
        }
        materials_->RebuildTextureSlotIndex();
        materials_->RewriteTextureSlot(it->old_slot, it->new_handle.slot);
        const auto old = bindless_->GetHandle(it->old_slot);
        if (old.has_value()) {
            bindless_->ReleaseSlot(*old, frame_index);
        }
        if (residency_ != nullptr) {
            if (old.has_value()) {
                residency_->Forget(*old);
            }
        }
        LOGIFACE_LOG(info, "TextureReloader: swapped slot " + std::to_string(it->old_slot) +
                               " -> " + std::to_string(it->new_handle.slot));
        it = pending_swaps_.erase(it);
        ++swapped;
    }

    // 2. Handle changed files. Match by canonical path so a watcher's path
    //    separators or relative directory do not defeat the lookup.
    const std::vector<std::string> changed = watcher_->PollChanged();
    for (const std::string& raw_path : changed) {
        std::error_code ec;
        const auto changed_path = std::filesystem::weakly_canonical(raw_path, ec);
        std::string matched_id;
        for (const auto& [id, watch] : watched_) {
            std::error_code watch_ec;
            const auto watched_path = std::filesystem::weakly_canonical(watch.path, watch_ec);
            if (!ec && !watch_ec && changed_path == watched_path) {
                matched_id = id;
                break;
            }
        }
        if (matched_id.empty()) {
            continue;
        }
        const std::string& path = raw_path;
        auto* resource = resources_->GetResource<TextureResource>(ResourceId{matched_id});
        if (resource == nullptr || !resource->ReloadFromPath(path)) {
            continue;
        }
        if (uploader_ == nullptr || materials_ == nullptr || bindless_ == nullptr) {
            continue;
        }
        // Find the slot the resource currently occupies, then re-upload into a
        // fresh slot; the swap is applied once the new binding publishes.
        const auto current = residency_ ? residency_->HandleForResource(ResourceId{matched_id})
                                        : std::nullopt;
        if (!current.has_value()) {
            continue;
        }
        const Watched& watch = watched_.at(matched_id);
        auto reservation = uploader_->Reserve(watch.semantic, watch.normal_encoding,
                                              watch.sampler, ResourceId{matched_id});
        if (!reservation.has_value()) {
            LOGIFACE_LOG(warn, "TextureReloader: capacity exhausted reloading '" + matched_id + "'");
            continue;
        }
        auto data = std::make_shared<TextureData>(resource->GetData());
        (void)uploader_->Submit(*reservation, std::move(data));
        pending_swaps_.push_back(PendingSwap{.old_slot = current->slot,
                                             .new_handle = reservation->handle});
    }
    return swapped;
}

} // namespace VulkanEngine::Textures
