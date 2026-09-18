module;

export module VulkanEngine.ShaderWatcher;

import std;
import VulkanEngine.ShaderManager;

export namespace VulkanEngine::ShaderSystem {

class ShaderWatcher {
public:
    explicit ShaderWatcher(ShaderManager& shaders);
    ~ShaderWatcher();

    ShaderWatcher(const ShaderWatcher&) = delete;
    ShaderWatcher& operator=(const ShaderWatcher&) = delete;

    void Start();
    void Stop();
    // Like Stop(), but drops the efsw FileWatcher (whose internal thread join can
    // block for its poll interval) on a detached thread. Safe only because Stop()
    // first quiesces every path that touches the shader manager; the detached
    // join owns no other resources. Never wait for completion afterwards.
    void StopAsync();

    // Invoked from the efsw listener thread (internal use). Public only because
    // the listener type lives in the implementation file.
    void OnFileChanged(const std::string& filename);

private:
    struct Impl;

    ShaderManager& shaders_;
    std::unique_ptr<Impl> impl_;
    std::mutex mutex_;
    std::unordered_map<std::string, std::filesystem::file_time_type> last_compile_;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> pending_reloads_;
    std::condition_variable cv_;
    std::thread debounce_thread_;
    std::atomic<bool> stop_{false};

    void DebounceLoop();
    void ReloadPath(const std::string& slang_path);
    // Sets the stop flag, clears pending reloads, wakes and joins the debounce
    // thread. After this returns, no efsw/debounce path can reach `shaders_`.
    void Quiesce();
};

} // namespace VulkanEngine::ShaderSystem
