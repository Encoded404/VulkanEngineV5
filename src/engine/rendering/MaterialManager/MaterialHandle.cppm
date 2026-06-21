module;

export module VulkanEngine.MaterialManager:MaterialHandle;

import std;
import VulkanEngine.TechniqueManager.TechniqueId;

// ── Design ──
// Lambda-based modify<T>([](T& d) { d.field = value; }) eliminates the
// reference-escape footgun that RAII proxy approaches have.  After the
// lambda returns, the dirty flag and per-binding dirty mask are set
// automatically, and MarkDirty() is called on the MaterialManager.
//
// Usage:
//   auto wood = material_mgr.Register<PBRTechnique>(BlendMode::Opaque, MaterialData{...});
//   wood.modify<MaterialData>([](auto& d) { d.roughness = 0.95f; });
//   float r = wood.read<MaterialData>().roughness;
//
// The technique type Tech must provide static constexpr members:
//   template<typename T> static constexpr std::size_t GetOffset()
//   template<typename T> static constexpr bool     HasBinding()
//   template<typename T> static constexpr std::uint32_t GetBindingIndex()

export namespace VulkanEngine::MaterialManager {

// ── Blend mode ──
enum class BlendMode : std::uint8_t {
    Opaque = 0,
    Cutout,
    Transparent
};

// ── Per-material GPU data entry ──
struct MaterialEntry {
    TechniqueManager::TechniqueId technique_id{0};
    BlendMode blend_mode{BlendMode::Opaque};
    bool dirty = false;
    std::uint32_t dirty_bindings = 0;
    std::vector<std::byte> cpu_data;  // PerMaterial bindings only, flat buffer
};

template<typename Tech>
class MaterialHandle {
public:
    // ── Read access: const ref, never dirties, constexpr offset ──
    template<typename T>
    const T& read() const {
        static_assert(Tech::template HasBinding<T>(),
                      "Technique does not declare a PerMaterial binding for this type");
        constexpr std::size_t off = Tech::template GetOffset<T>();
        return *reinterpret_cast<const T*>(entry_->cpu_data.data() + off);
    }

    // ── Write access: lambda-based, no reference escape possible ──
    // After func() returns: dirty flag set, per-binding dirty mask updated, MarkDirty called.
    template<typename T, typename Func>
    void modify(Func&& func) {
        static_assert(Tech::template HasBinding<T>(),
                      "Technique does not declare a PerMaterial binding for this type");
        constexpr std::size_t off = Tech::template GetOffset<T>();
        constexpr std::uint32_t binding_idx = Tech::template GetBindingIndex<T>();
        func(*reinterpret_cast<T*>(entry_->cpu_data.data() + off));
        entry_->dirty = true;
        entry_->dirty_bindings |= (1u << binding_idx);
        mark_dirty_(id_);
    }

    // ── Accessors ──
    [[nodiscard]] std::uint32_t id() const { return id_; }
    [[nodiscard]] bool valid() const { return entry_ != nullptr; }

    // Copyable — all copies point to the same MaterialEntry
    MaterialHandle(const MaterialHandle&) = default;
    MaterialHandle& operator=(const MaterialHandle&) = default;
    MaterialHandle(MaterialHandle&&) = default;
    MaterialHandle& operator=(MaterialHandle&&) = default;

private:
    friend class MaterialManager;

    MaterialHandle(std::uint32_t id, MaterialEntry* entry,
                   void (*mark_dirty)(std::uint32_t))
        : id_(id), entry_(entry), mark_dirty_(mark_dirty) {}

    std::uint32_t id_ = 0;
    MaterialEntry* entry_ = nullptr;
    void (*mark_dirty_)(std::uint32_t) = nullptr;
};

} // namespace VulkanEngine::MaterialManager
