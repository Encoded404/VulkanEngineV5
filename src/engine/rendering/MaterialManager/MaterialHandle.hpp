// MaterialHandle.hpp — Templated material handle with lambda-based modify<T>() and read<T>().
//
// This is a header-only template because it's parameterized on the technique type (Tech),
// which is user-defined and not known to the engine module.
//
// Design: Lambda-based modify<T>([](T& d) { d.field = value; }) eliminates reference-escape
// footgun that RAII proxy approaches have. After the lambda returns, dirty flag and per-binding
// dirty mask are set automatically, and MarkDirty() is called on the MaterialManager.
//
// Usage:
//   auto wood = material_mgr.Register<PBRTechnique>(BlendMode::Opaque, MaterialData{...});
//   wood.modify<MaterialData>([](auto& d) { d.roughness = 0.95f; });
//   float r = wood.read<MaterialData>().roughness;
//
// The technique type Tech must provide static constexpr members:
//   template<typename T> static constexpr size_t GetOffset()
//   template<typename T> static constexpr bool HasBinding()
//   template<typename T> static constexpr uint32_t GetBindingIndex()

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>
#include <type_traits>

namespace VulkanEngine::MaterialManager {

// Forward declaration of MaterialEntry (defined in the module)
struct MaterialEntry;

template<typename Tech>
class MaterialHandle {
public:
    // ── Read access: const ref, never dirties, constexpr offset ──
    template<typename T>
    const T& read() const {
        static_assert(Tech::template HasBinding<T>(),
                      "Technique does not declare a PerMaterial binding for this type");
        constexpr size_t off = Tech::template GetOffset<T>();
        return *reinterpret_cast<const T*>(entry_->cpu_data.data() + off);
    }

    // ── Write access: lambda-based, no reference escape possible ──
    // After func() returns: dirty flag set, per-binding dirty mask updated, MarkDirty called.
    template<typename T, typename Func>
    void modify(Func&& func) {
        static_assert(Tech::template HasBinding<T>(),
                      "Technique does not declare a PerMaterial binding for this type");
        constexpr size_t off = Tech::template GetOffset<T>();
        constexpr uint32_t binding_idx = Tech::template GetBindingIndex<T>();
        func(*reinterpret_cast<T*>(entry_->cpu_data.data() + off));
        entry_->dirty = true;
        entry_->dirty_bindings |= (1u << binding_idx);
        mark_dirty_(id_);
    }

    // ── Accessors ──
    uint32_t id() const { return id_; }
    bool valid() const { return entry_ != nullptr; }

    // Copyable — all copies point to the same MaterialEntry
    MaterialHandle(const MaterialHandle&) = default;
    MaterialHandle& operator=(const MaterialHandle&) = default;
    MaterialHandle(MaterialHandle&&) = default;
    MaterialHandle& operator=(MaterialHandle&&) = default;

private:
    friend class MaterialManager;

    MaterialHandle(uint32_t id, MaterialEntry* entry,
                   void (*mark_dirty)(uint32_t))
        : id_(id), entry_(entry), mark_dirty_(mark_dirty) {}

    uint32_t id_ = 0;
    MaterialEntry* entry_ = nullptr;
    void (*mark_dirty_)(uint32_t) = nullptr;
};

} // namespace VulkanEngine::MaterialManager
