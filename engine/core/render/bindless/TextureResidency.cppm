module;

export module VulkanEngine.TextureResidency;

import std;
import std.compat;

import VulkanBackend.Vulkan.VulkanBootstrap;
import VulkanBackend.Vulkan.VulkanCapabilities;
import VulkanEngine.BindlessManager;
import VulkanEngine.BindlessManager.TextureSlot;
import VulkanEngine.GpuResources;
import VulkanEngine.GpuTexture;
import VulkanEngine.ResidencyPolicy;
import VulkanEngine.ResourceSystem;
import VulkanEngine.MaterialManager;

export namespace VulkanEngine::Textures {

// Whole-texture residency under a device memory budget.
//
// Tracks every uploaded texture by bindless handle + byte size and evicts
// least-recently-used whole textures when the resident set exceeds the budget.
// Eviction goes through the normal deferred release path: the slot's descriptor
// is reset to the fallback at the bindless ring drain (so no in-flight frame
// ever samples a destroyed image) and every material referencing the slot is
// rewritten to the fallback slot, matching what the shader already does for a
// not-yet-resident upload.
//
// The budget is the VK_EXT_memory_budget heap budget when the extension is
// present, otherwise a caller-supplied byte budget; with neither, eviction is
// disabled (budget 0) and the engine behaves exactly as before.
class TextureResidency {
public:
    TextureResidency() = default;

    void Initialize(BindlessManager::BindlessManager& bindless,
                    MaterialManager::MaterialManager& materials,
                    const VulkanBackend::Vulkan::VulkanCapabilities& capabilities,
                    std::uint64_t fallback_budget_bytes = 0);
    void Shutdown();

    // Records a texture reservation (its slot is pre-bound to the fallback).
    // It becomes an evictable candidate only after MarkResident.
    void TrackReservation(BindlessManager::TextureHandle handle, std::uint64_t bytes,
                          const ResourceId& id, std::uint64_t frame, bool pinned = false);

    // The real binding published: the texture is now a resident candidate.
    void MarkResident(BindlessManager::TextureHandle handle, std::uint64_t bytes,
                      std::uint64_t frame);

    // A handle the frame sampled: refreshes its LRU age.
    void Touch(BindlessManager::TextureHandle handle, std::uint64_t frame);

    // Drops a handle from residency (a destroyed/unloaded texture).
    void Forget(BindlessManager::TextureHandle handle);

    // ── Resource -> handle registry (hot reload) ──
    // Records which bindless handle a resource id currently occupies. A
    // resource uploads once; a later upload of the same id replaces the entry
    // (the caller retires/releases the previous handle).
    void RegisterResourceHandle(const ResourceId& id, BindlessManager::TextureHandle handle,
                                std::uint64_t frame);
    // The handle a resource currently occupies, or nullopt. The returned handle
    // is generation-checked by the caller through the bindless manager.
    [[nodiscard]] std::optional<BindlessManager::TextureHandle> HandleForResource(
        const ResourceId& id) const;
    void ForgetResource(const ResourceId& id);
    // Snapshots the resource ids and handles currently registered (hot-reload
    // scan). Order is unspecified.
    [[nodiscard]] std::vector<std::pair<ResourceId, BindlessManager::TextureHandle>>
    RegisteredResources() const;

    // Runs one LRU eviction pass against the budget. Returns the number of
    // textures evicted. No-op when the budget is 0 (extension absent and no
    // override) or the set already fits.
    std::uint32_t Collect(std::uint32_t frame,
                          std::uint64_t min_residency_frames = 0);

    void SetBudgetBytes(std::uint64_t bytes) { budget_override_ = bytes; }
    [[nodiscard]] std::uint64_t BudgetBytes() const { return EffectiveBudget(); }
    [[nodiscard]] std::uint64_t ResidentBytes() const { return set_.ResidentBytes(); }
    [[nodiscard]] std::size_t TrackedCount() const { return set_.Size(); }
    [[nodiscard]] bool IsEnabled() const { return EffectiveBudget() != 0; }

private:
    struct Key {
        std::uint32_t slot{0};
        std::uint32_t generation{0};
        bool operator==(const Key&) const = default;
    };
    struct KeyHash {
        std::size_t operator()(const Key& key) const noexcept {
            return (static_cast<std::size_t>(key.slot) << 32U) ^ key.generation;
        }
    };

    [[nodiscard]] std::uint64_t EffectiveBudget() const;

    BindlessManager::BindlessManager* bindless_ = nullptr;
    MaterialManager::MaterialManager* materials_ = nullptr;
    VulkanBackend::Vulkan::VulkanCapabilities const* capabilities_ = nullptr;

    Residency::ResidencySet set_{};
    // handle -> opaque residency id (the map key above is not stable across
    // generation reuse, so the set is keyed by a monotonically assigned id).
    std::unordered_map<Key, std::uint64_t, KeyHash> key_to_id_{};
    // residency id -> handle, to act on an eviction plan.
    std::unordered_map<std::uint64_t, BindlessManager::TextureHandle> id_to_handle_{};
    std::uint64_t next_id_ = 1;

    std::uint64_t budget_override_ = 0;
    std::uint64_t extension_budget_ = 0;

    std::unordered_map<std::string, BindlessManager::TextureHandle> resource_handles_{};
};

} // namespace VulkanEngine::Textures
