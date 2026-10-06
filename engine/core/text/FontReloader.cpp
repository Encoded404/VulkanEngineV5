module;

#include <logging/logging_macros.hpp>

module VulkanEngine.Text.FontReloader;

import std;
import std.compat;

import logiface;

import VulkanEngine.ResourceSystem;
import VulkanEngine.Text.FontWatcher;
import VulkanEngine.Text.TextSystem;

namespace VulkanEngine::Text {

std::string FontReloader::Key(const ResourceId& id, std::uint32_t face_index) {
    return id.value + "#" + std::to_string(face_index);
}

void FontReloader::Initialize(FontWatcher& watcher, TextSystem& text_system) {
    watcher_ = &watcher;
    text_system_ = &text_system;
}

void FontReloader::Shutdown() {
    watched_.clear();
    watcher_ = nullptr;
    text_system_ = nullptr;
}

void FontReloader::WatchResource(const ResourceId& id, const std::filesystem::path& path,
                                 std::uint32_t face_index) {
    if (watcher_ == nullptr || path.empty()) {
        return;
    }
    Watched watch{};
    watch.id = id;
    watch.path = path;
    watch.face_index = face_index;
    watched_[Key(id, face_index)] = std::move(watch);
    (void)watcher_->AddDirectory(path.parent_path().string());
}

void FontReloader::UnwatchResource(const ResourceId& id, std::uint32_t face_index) {
    watched_.erase(Key(id, face_index));
}

std::uint32_t FontReloader::Pump(std::uint32_t frame_index) {
    if (watcher_ == nullptr || text_system_ == nullptr) {
        return 0;
    }

    std::uint32_t reloaded = 0;
    const std::vector<std::string> changed = watcher_->PollChanged();
    for (const std::string& raw_path : changed) {
        // Match by canonical path so a watcher's separators or relative
        // directory do not defeat the lookup.
        std::error_code changed_ec;
        const auto changed_path = std::filesystem::weakly_canonical(raw_path, changed_ec);
        for (const auto& [key, watch] : watched_) {
            std::error_code watch_ec;
            const auto watched_path = std::filesystem::weakly_canonical(watch.path, watch_ec);
            if (changed_ec || watch_ec || changed_path != watched_path) {
                continue;
            }
            (void)key;
            if (text_system_->ReloadFont(watch.id, raw_path, frame_index, watch.face_index)) {
                LOGIFACE_LOG(info, "FontReloader: reloaded '" + watch.id.value + "'");
                ++reloaded;
            }
            break;
        }
    }
    return reloaded;
}

} // namespace VulkanEngine::Text
