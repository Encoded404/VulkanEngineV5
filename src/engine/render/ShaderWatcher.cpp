module;

#include <logging/logging_macros.hpp>

#if defined(__linux__)
#include <sys/inotify.h>
#include <unistd.h>
#include <poll.h>
#endif

module VulkanEngine.ShaderWatcher;

import std;
import std.compat;

import logiface;

import VulkanEngine.ShaderManager;

namespace VulkanEngine::ShaderSystem {

ShaderWatcher::ShaderWatcher(ShaderManager& shaders)
    : shaders_(shaders)
{}

void ShaderWatcher::Watch(ShaderId id) {
    auto& slot = shaders_.GetSlot(id);
    Entry entry{};
    entry.id = id;
    entry.last_write = std::filesystem::last_write_time(slot.slang_path);
    entries_.push_back(entry);
}

void ShaderWatcher::Poll() {
    for (auto& entry : entries_) {
        auto& slot = shaders_.GetSlot(entry.id);
        auto ftime = std::filesystem::last_write_time(slot.slang_path);
        if (ftime > entry.last_write) {
            entry.last_write = ftime;
            shaders_.RequestReload(entry.id);
        }
    }
}

#if defined(__linux__)

void ShaderWatcher::Start() {
    running_.store(true, std::memory_order_release);

    inotify_fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inotify_fd_ < 0) {
        LOGIFACE_LOG(warn, "ShaderWatcher: inotify_init1 failed, using polling fallback");
        return;
    }

    // inotify events for file watches carry an empty `name` — match events to
    // shaders by watch descriptor, not by filename.
    std::unordered_map<int, ShaderId> wd_to_id;
    for (auto& entry : entries_) {
        auto& slot = shaders_.GetSlot(entry.id);
        int wd = inotify_add_watch(inotify_fd_, slot.slang_path.c_str(),
                                     IN_CLOSE_WRITE | IN_MODIFY);
        if (wd >= 0) {
            wd_to_id.emplace(wd, entry.id);
        } else {
            LOGIFACE_LOG(warn, "ShaderWatcher: failed to watch " + slot.slang_path);
        }
    }

    background_thread_ = std::jthread([this, wd_to_id = std::move(wd_to_id)]() {
        while (running_.load(std::memory_order_acquire)) {
            struct pollfd pfd{};
            pfd.fd = inotify_fd_;
            pfd.events = POLLIN;
            int ret = poll(&pfd, 1, 500);
            if (ret <= 0) continue;

            char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
            ssize_t len = read(inotify_fd_, buf, sizeof(buf));
            if (len <= 0) continue;

            for (const char* ptr = buf; ptr < buf + len; ) {
                auto* event = reinterpret_cast<const struct inotify_event*>(ptr);
                ptr += sizeof(struct inotify_event) + event->len;

                if (event->mask & (IN_CLOSE_WRITE | IN_MODIFY)) {
                    auto it = wd_to_id.find(event->wd);
                    if (it != wd_to_id.end()) {
                        shaders_.RequestReload(it->second);
                    }
                }
            }
        }
        close(inotify_fd_);
        inotify_fd_ = -1;
    });
}

void ShaderWatcher::Stop() {
    running_.store(false, std::memory_order_release);
    if (background_thread_.joinable()) {
        background_thread_.join();
    }
}

#else // !__linux__

void ShaderWatcher::Start() {
    LOGIFACE_LOG(info, "ShaderWatcher: platform-native watching not available, using polling");
}

void ShaderWatcher::Stop() {
    running_.store(false, std::memory_order_release);
}

#endif

} // namespace VulkanEngine::ShaderSystem
