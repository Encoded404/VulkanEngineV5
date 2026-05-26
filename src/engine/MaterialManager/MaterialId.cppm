module;

#include <cstdint>

export module VulkanEngine.MaterialId;

export namespace VulkanEngine {

struct MaterialId {
    explicit MaterialId(uint16_t v) : value(v) {}
    uint16_t value; // NOLINT(misc-non-private-member-variables-in-classes)
    bool operator==(const MaterialId& o) const = default;
};

} // namespace VulkanEngine
