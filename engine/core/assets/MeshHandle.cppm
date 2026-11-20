module;

export module VulkanEngine.MeshHandle;

import std;

export namespace VulkanEngine {

// Identifies one registration in the mesh registry.
// `slot` indexes the registry's entry vector, and `generation` counts the registrations that slot has held.
// A handle taken from a released mesh therefore never matches the mesh that later reuses the slot, which a bare index cannot promise.
struct MeshHandle {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::uint32_t slot = 0;
    std::uint32_t generation = 0;
    // NOLINTEND(misc-non-private-member-variables-in-classes)

    bool operator==(const MeshHandle&) const = default;

    // A live slot starts at generation 1, so a default-constructed handle names no mesh.
    [[nodiscard]] bool IsValid() const { return generation != 0; }

    // Packs both fields into one value for the once-per-cause log signatures.
    // The packing is a bijection, so two different handles never hash the same.
    [[nodiscard]] std::uint64_t Signature() const {
        return (static_cast<std::uint64_t>(generation) << 32) | static_cast<std::uint64_t>(slot);
    }
};

} // namespace VulkanEngine
