module;

export module VulkanEngine.Text.FontReloader;

import std;

import VulkanEngine.ResourceSystem;
import VulkanEngine.Text.FontWatcher;
import VulkanEngine.Text.TextSystem;

export namespace VulkanEngine::Text {

// Hot-reloads fonts without invalidating a frame that is already recorded.
//
// Mirrors TextureReloader: the watcher reports debounced paths on the main
// thread, this class maps a path back to the resource it was registered for,
// and then hands the work to TextSystem. ReloadFont re-reads the resource,
// rebuilds the face (a new UniqueId) and invalidates the shaping cache, the
// glyph cache and the CPU atlas, retiring the GPU pages through the bindless
// ring drain. A frame that already recorded text against an old page therefore
// keeps sampling a live image for its whole lifetime; only frames recorded after
// the reload see the new one.
//
// A rejected or missing file is a no-op at every level: the resource keeps its
// bytes and version, the face keeps its identity, and no cache entry is touched.
class FontReloader {
public:
    void Initialize(FontWatcher& watcher, TextSystem& text_system);
    void Shutdown();

    // Registers a source file for a font resource so a change to it triggers a
    // reload. The directory is added to the watcher.
    void WatchResource(const ResourceId& id, const std::filesystem::path& path,
                       std::uint32_t face_index = 0);

    // Stops watching a resource (unloaded font).
    void UnwatchResource(const ResourceId& id, std::uint32_t face_index = 0);

    // Main-thread per-frame pump: drains the watcher and reloads the fonts whose
    // files changed. Returns the number of fonts reloaded this call.
    std::uint32_t Pump(std::uint32_t frame_index);

    [[nodiscard]] std::size_t WatchedCount() const { return watched_.size(); }

private:
    struct Watched {
        // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
        ResourceId id;
        std::filesystem::path path{};
        std::uint32_t face_index = 0;
        // NOLINTEND(misc-non-private-member-variables-in-classes)
    };

    // Keyed by resource id and face index: one file can back several faces.
    [[nodiscard]] static std::string Key(const ResourceId& id, std::uint32_t face_index);

    FontWatcher* watcher_ = nullptr;
    TextSystem* text_system_ = nullptr;
    std::unordered_map<std::string, Watched> watched_{};
};

} // namespace VulkanEngine::Text
