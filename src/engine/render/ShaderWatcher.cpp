module;

#include <logging/logging_macros.hpp>

#include <efsw/efsw.hpp>

module VulkanEngine.ShaderWatcher;

import std;
import std.compat;

import logiface;

import VulkanEngine.ShaderManager;

namespace VulkanEngine::ShaderSystem {

namespace {

struct WatcherListener final : efsw::FileWatchListener {
    ShaderWatcher& owner;

    explicit WatcherListener(ShaderWatcher& owner_ref)
        : owner(owner_ref)
    {}

    void handleFileAction(efsw::WatchID /*watchid*/, const std::string& /*dir*/,
                          const std::string& filename, efsw::Action action,
                          std::string /*oldFilename*/) override {
        if (action != efsw::Actions::Modified && action != efsw::Actions::Add) return;
        if (!filename.ends_with(".slang")) return; // ignore temp-file artifacts
        owner.OnFileChanged(filename);
    }
};

// inotify reports several events per save (write, chmod, close), and editors
// that save atomically may write the file twice (temp file + rename). Debounce
// so a single save collapses into exactly one reload after the burst settles.
inline constexpr auto kReloadDebounce = std::chrono::milliseconds(200);

} // anonymous namespace

struct ShaderWatcher::Impl {
    // Destroyed in reverse declaration order: the watcher (joining its thread)
    // is destroyed before the listener.
    std::unique_ptr<WatcherListener> listener;
    std::unique_ptr<efsw::FileWatcher> watcher;
};

ShaderWatcher::ShaderWatcher(ShaderManager& shaders)
    : shaders_(shaders)
{}

ShaderWatcher::~ShaderWatcher() {
    Stop();
}

void ShaderWatcher::Start() {
    if (impl_) return;
    impl_ = std::make_unique<Impl>();
    impl_->listener = std::make_unique<WatcherListener>(*this);
    impl_->watcher = std::make_unique<efsw::FileWatcher>(); // native backend (inotify on Linux)

    for (const auto& dir : shaders_.GetSlangDirectories()) {
        const auto watch_id = impl_->watcher->addWatch(dir, impl_->listener.get(), /*recursive=*/false);
        if (watch_id < 0) {
            LOGIFACE_LOG(warn, "ShaderWatcher: failed to watch " + dir);
        } else {
            LOGIFACE_LOG(info, "ShaderWatcher: watching " + dir);
        }
    }
    impl_->watcher->watch();

    stop_.store(false, std::memory_order_release);
    debounce_thread_ = std::thread([this] { DebounceLoop(); });
}

void ShaderWatcher::Stop() {
    {
        std::unique_lock lock(mutex_);
        stop_.store(true, std::memory_order_release);
        pending_reloads_.clear();
    }
    cv_.notify_all();
    if (debounce_thread_.joinable()) {
        debounce_thread_.join();
    }
    impl_.reset(); // ~FileWatcher stops and joins its internal thread
}

void ShaderWatcher::OnFileChanged(const std::string& filename) {
    const auto id = shaders_.FindBySlangFilename(filename);
    if (id == static_cast<ShaderId>(-1)) return;

    const auto& slot = shaders_.GetSlot(id);
    const std::string slang_path = slot.slang_path;
    LOGIFACE_LOG(debug, "ShaderWatcher: file changed: " + slang_path);

    {
        std::unique_lock lock(mutex_);
        pending_reloads_[slang_path] = std::chrono::steady_clock::now() + kReloadDebounce;
    }
    cv_.notify_one();
}

void ShaderWatcher::DebounceLoop() {
    while (true) {
        std::vector<std::string> due;
        {
            std::unique_lock lock(mutex_);
            if (stop_.load(std::memory_order_acquire)) return;
            if (pending_reloads_.empty()) {
                cv_.wait(lock);
                continue;
            }
            const auto earliest = std::ranges::min_element(
                pending_reloads_, [](const auto& a, const auto& b) { return a.second < b.second; });
            cv_.wait_until(lock, earliest->second);
            if (stop_.load(std::memory_order_acquire)) return;
            const auto now = std::chrono::steady_clock::now();
            for (auto it = pending_reloads_.begin(); it != pending_reloads_.end();) {
                if (it->second <= now) {
                    due.push_back(it->first);
                    it = pending_reloads_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (const auto& path : due) {
            ReloadPath(path);
        }
    }
}

void ShaderWatcher::ReloadPath(const std::string& slang_path) {
    std::filesystem::file_time_type ftime;
    try {
        ftime = std::filesystem::last_write_time(slang_path);
    } catch (const std::filesystem::filesystem_error&) {
        return; // file may have been renamed away; a later event re-schedules
    }

    {
        std::unique_lock lock(mutex_);
        const auto it = last_compile_.find(slang_path);
        if (it != last_compile_.end() && ftime <= it->second) return; // duplicate event
        last_compile_[slang_path] = ftime;
    }
    const auto id = shaders_.FindBySlangPath(slang_path);
    if (id == static_cast<ShaderId>(-1)) return;
    shaders_.RequestReload(id);
}

} // namespace VulkanEngine::ShaderSystem
