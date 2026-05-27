module;

#include <cstdint>

export module VulkanEngine.MaterialManager.MaterialId;

export namespace VulkanEngine::MaterialManager {

struct MaterialId {
    MaterialId() = default;
    explicit MaterialId(uint16_t v) : value(v) {}
    uint16_t value{0}; // NOLINT(misc-non-private-member-variables-in-classes)
    bool operator==(const MaterialId& o) const = default;
};

} // namespace VulkanEngine::MaterialManager
