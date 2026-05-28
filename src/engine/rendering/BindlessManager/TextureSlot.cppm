module;

#include <cstdint>

export module VulkanEngine.BindlessManager.TextureSlot;

export namespace VulkanEngine::BindlessManager {

struct TextureSlot {
    TextureSlot() = default;
    explicit TextureSlot(uint16_t v) : value(v) {}
    uint16_t value{0}; // NOLINT(misc-non-private-member-variables-in-classes)
    bool operator==(const TextureSlot& o) const = default;
};

} // namespace VulkanEngine::BindlessManager
