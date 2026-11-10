module;

export module VulkanEngine.ModifierId;

import std;

export namespace VulkanEngine::Modifiers {

// Handle to one entry in a ModifierManager registry.
//
// `value` indexes the manager's slot array; `generation` changes every time the
// slot is recycled, so a stale handle is detected instead of silently naming a
// different modifier. Both default to the invalid state, which makes a
// value-initialized ModifierId the invalid handle.
inline constexpr std::uint32_t kInvalidModifierValue = 0xFFFFFFFFu;

struct ModifierId {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::uint32_t value = kInvalidModifierValue;
    std::uint32_t generation = 0;
    // NOLINTEND(misc-non-private-member-variables-in-classes)

    [[nodiscard]] bool IsValid() const noexcept { return value != kInvalidModifierValue; }

    auto operator<=>(const ModifierId&) const = default;
};

inline constexpr ModifierId kInvalidModifierId{};

} // namespace VulkanEngine::Modifiers
