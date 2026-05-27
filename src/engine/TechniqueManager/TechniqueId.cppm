module;

#include <cstdint>

export module VulkanEngine.TechniqueManager.TechniqueId;

export namespace VulkanEngine::TechniqueManager {

struct TechniqueId {
    TechniqueId() = default;
    explicit TechniqueId(uint16_t v) : value(v) {}
    uint16_t value{0}; // NOLINT(misc-non-private-member-variables-in-classes)
    bool operator==(const TechniqueId& o) const = default;
};

} // namespace VulkanEngine::TechniqueManager
