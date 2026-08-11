module;

export module VulkanEngine.ShaderWatcher;

import std;
import VulkanEngine.ShaderManager;

export namespace VulkanEngine::ShaderSystem {

class ShaderWatcher {
public:
    explicit ShaderWatcher(ShaderManager& shaders);

    ShaderWatcher(const ShaderWatcher&) = delete;
    ShaderWatcher& operator=(const ShaderWatcher&) = delete;

    void Watch(ShaderId id);
    void Poll();
    void Start();
    void Stop();

private:
    struct Entry {
        ShaderId id;
        std::filesystem::file_time_type last_write;
    };

    ShaderManager& shaders_;
    std::vector<Entry> entries_;
    std::atomic<bool> running_{false};
    int inotify_fd_ = -1;
    std::jthread background_thread_;
};

} // namespace VulkanEngine::ShaderSystem
