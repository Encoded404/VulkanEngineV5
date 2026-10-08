// Entity identity primitives for the hybrid ECS.
//
// This module holds only identity: the generation-checked entity handle, the
// component-type budget, and the per-type id source. Component, Entity, the
// component pools and the registry live in VulkanEngine.ECS.ComponentRegistry
// because Entity::GetComponent<T> needs the registry complete, which would be a
// module cycle if Entity were defined here.

module;

export module VulkanEngine.ECS.Entity;

import std;

export namespace VulkanEngine {

// Persistent entity handle. This is the reference you may store across frames;
// `Entity&` is only valid until the next structural change.
//
// `generation` guards slot reuse: destroying an entity bumps the slot's
// generation, so a handle captured before the destroy can never resolve to the
// entity that later occupies the same slot.
struct EntityId {
    std::uint32_t index = 0;
    std::uint32_t generation = 0;

    [[nodiscard]] constexpr bool IsValid() const noexcept { return generation != 0; }

    friend constexpr bool operator==(const EntityId&, const EntityId&) noexcept = default;
};

// One mask bit per registered component type, so this is also the component
// type budget. Exceeding it is reported by ComponentRegistry, not silently
// truncated.
inline constexpr std::size_t kMaxComponentTypes = 64;
using ComponentMask = std::bitset<kMaxComponentTypes>;

// Component type ID system. IDs are stable per type for the process lifetime.
class ComponentTypeIDSystem {
private:
    inline static std::atomic_size_t next_type_id{0};

public:
    template<typename T>
    [[nodiscard]] static std::size_t GetTypeID() {
        static const std::size_t type_id = next_type_id.fetch_add(1, std::memory_order_relaxed);
        return type_id;
    }

    [[nodiscard]] static std::size_t RegisteredTypeCount() noexcept {
        return next_type_id.load(std::memory_order_relaxed);
    }
};

} // namespace VulkanEngine
