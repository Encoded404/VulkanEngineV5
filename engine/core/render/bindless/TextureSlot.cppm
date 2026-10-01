module;

export module VulkanEngine.BindlessManager.TextureSlot;

import std;

export namespace VulkanEngine::BindlessManager {

// Legacy fixed-width slot alias (part of the material/asset CPU types).
struct TextureSlot {
    TextureSlot() = default;
    explicit TextureSlot(std::uint16_t v) : value(v) {}
    std::uint16_t value{0}; // NOLINT(misc-non-private-member-variables-in-classes)
    bool operator==(const TextureSlot& o) const = default;
};

// Generation-checked bindless handle. Defined in this dependency-free leaf
// module so `assets` and `render` can name a handle without importing the
// BindlessManager service module. A slot index alone cannot protect against
// reuse; the generation rejects a stale op or a stale CPU reference after the
// slot has been released and re-reserved.
struct TextureHandle {
    std::uint32_t slot{0};
    std::uint32_t generation{0};

    [[nodiscard]] bool IsValid() const noexcept { return generation != 0; }
    bool operator==(const TextureHandle&) const = default;
};

} // namespace VulkanEngine::BindlessManager
