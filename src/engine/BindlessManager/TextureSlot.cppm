module;

#include <cstdint>

export module VulkanEngine.TextureSlot;

export namespace VulkanEngine {

struct TextureSlot {
    explicit TextureSlot(uint16_t v) : value(v) {}
    uint16_t value; // NOLINT(misc-non-private-member-variables-in-classes)
    bool operator==(const TextureSlot& o) const = default;
};

} // namespace VulkanEngine
