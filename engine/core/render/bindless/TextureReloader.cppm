module;

export module VulkanEngine.TextureReloader;

import std;

import VulkanEngine.ResourceSystem;
import VulkanEngine.ResourceSystem.TextureResource;
import VulkanEngine.BindlessManager;
import VulkanEngine.BindlessManager.TextureSlot;
import VulkanEngine.TextureUploader;
import VulkanEngine.TextureResidency;
import VulkanEngine.TextureWatcher;
import VulkanEngine.MaterialManager;
import VulkanEngine.TextureTypes;

export namespace VulkanEngine::Textures {

// Hot-reloads textures without stalling in-flight frames.
//
// A changed image file is re-decoded on the main thread and uploaded into a
// *new* bindless slot through the normal async uploader. The old slot keeps its
// binding until the new slot publishes at the bindless ring drain; then every
// material referencing the old slot is rewritten to the new slot and the old
// slot is released. A frame that already recorded draws against the old slot
// therefore samples a live image for its whole lifetime — the swap is only
// visible to frames recorded after the new binding published.
//
// Reloads are keyed by resource id: the watcher reports a path, the resource
// system reloads the payload, and the handle registry finds the slot it
// currently occupies.
class TextureReloader {
public:
    void Initialize(TextureSystem::TextureWatcher& watcher,
                    ResourceManager& resources,
                    BindlessManager::BindlessManager& bindless,
                    TextureUploader& uploader,
                    TextureResidency& residency,
                    MaterialManager::MaterialManager& materials);
    void Shutdown();

    // Registers a source file for a resource so a change to it triggers a
    // reload. Called when a texture is first loaded. The directory is added to
    // the watcher.
    void WatchResource(const ResourceId& id, const std::filesystem::path& path,
                       TextureSemantic semantic, TextureNormalEncoding normal_encoding,
                       const SamplerDesc& sampler);

    // Stops watching a resource (unloaded texture).
    void UnwatchResource(const ResourceId& id);

    // Main-thread per-frame pump: drains the watcher, reloads changed payloads,
    // re-uploads them, and applies any completed swaps. Returns the number of
    // textures swapped this call.
    std::uint32_t Pump(std::uint32_t frame_index);

    [[nodiscard]] std::size_t PendingSwapCount() const { return pending_swaps_.size(); }

private:
    struct Watched {
        std::filesystem::path path{};
        TextureSemantic semantic{TextureSemantic::Unknown};
        TextureNormalEncoding normal_encoding{TextureNormalEncoding::Standard};
        SamplerDesc sampler{};
    };
    struct PendingSwap {
        std::uint32_t old_slot{0};
        BindlessManager::TextureHandle new_handle{};
    };

    TextureSystem::TextureWatcher* watcher_ = nullptr;
    ResourceManager* resources_ = nullptr;
    BindlessManager::BindlessManager* bindless_ = nullptr;
    TextureUploader* uploader_ = nullptr;
    TextureResidency* residency_ = nullptr;
    MaterialManager::MaterialManager* materials_ = nullptr;

    std::unordered_map<std::string, Watched> watched_{};
    std::vector<PendingSwap> pending_swaps_{};
};

} // namespace VulkanEngine::Textures
