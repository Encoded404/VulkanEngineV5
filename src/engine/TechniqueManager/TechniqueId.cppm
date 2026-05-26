module;

#include <cstdint>

export module VulkanEngine.TechniqueId;

export namespace VulkanEngine {

struct TechniqueId {
    explicit TechniqueId(uint16_t v) : value(v) {}
    uint16_t value; // NOLINT(misc-non-private-member-variables-in-classes)
    bool operator==(const TechniqueId& o) const = default;
};

} // namespace VulkanEngine
