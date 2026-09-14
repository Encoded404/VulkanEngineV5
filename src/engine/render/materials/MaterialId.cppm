module;

export module VulkanEngine.MaterialManager.MaterialId;

import std;

export namespace VulkanEngine::MaterialManager {

struct MaterialId {
    MaterialId() = default;
    explicit MaterialId(std::uint32_t v) : value(v) {}
    std::uint32_t value{0}; // NOLINT(misc-non-private-member-variables-in-classes)
    bool operator==(const MaterialId& o) const = default;
};

inline constexpr std::uint32_t kInvalidMaterialId =
    std::numeric_limits<std::uint32_t>::max();

// Generation-checked reference to a material slot.
//
// `value` is the MaterialId index; `generation` is the value of that slot's
// generation counter at the time the reference was taken. A reference is stale
// if the manager's generation for `value` no longer matches — which catches
// both a destroyed slot and a freed slot that has since been reused by a
// different material. Comparing MaterialId alone cannot detect slot reuse.
struct MaterialRef {
    std::uint32_t value{kInvalidMaterialId}; // NOLINT(misc-non-private-member-variables-in-classes)
    std::uint32_t generation{0}; // NOLINT(misc-non-private-member-variables-in-classes)

    bool operator==(const MaterialRef& o) const = default;

    [[nodiscard]] bool IsSet() const { return value != kInvalidMaterialId; }

    static MaterialRef Unset() { return MaterialRef{}; }
};

} // namespace VulkanEngine::MaterialManager
