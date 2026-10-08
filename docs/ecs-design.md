# Hybrid ECS: contract and rules

The engine's ECS is deliberately hybrid: an entity can carry plain data that
systems iterate in bulk, and object-like components that own resources and drive
themselves. This document is the contract. Code lives in
`engine/core/ecs/Entity.cppm` (identity) and
`engine/core/ecs/ComponentRegistry.cppm` (everything else); every consumer
imports `VulkanEngine.ECS.ComponentRegistry`.

## The one rule

**Deriving from `Component` declares object-ness. Not deriving declares
data-ness.**

| | Object component | Data component |
|---|---|---|
| Declaration | `class X : Component` | plain `struct`/`class` |
| Storage | stable heap object (`unique_ptr<T>`) | dense contiguous column |
| Addressability | `X*` valid for the entity's life | **transient**: valid until the next structural change |
| Behavior | virtual `OnAttach` / `OnDetach` / `Update` | none; driven by systems/views |
| Long-lived reference | `X*` is fine | store `EntityId`, re-fetch with `GetComponent<T>` |
| Engine resources | RAII + `OnDetach` | via registry `OnAdd`/`OnRemove` hooks |

`Transform`, `MeshReference`, `OrmOverride`, `Text`, `Camera`,
`MaterialOverride` and `DynamicMesh` are data components. `Component` subclasses
in games (for example the basic-scene controller) are object components.

The old field-reflection / `FieldHandle` machinery is gone. Data-component
members are plain values; there is nothing to bind and nothing to keep in sync
when a column grows.

## Identity and lifetime

- `EntityId{index, generation}` is the **persistent** handle. It is
  generation-checked: destroying an entity bumps its slot's generation, so a
  handle captured before the destroy can never resolve to whatever later
  occupies the slot.
- `Entity&`/`Entity*` is valid until the next structural change. Its address
  stays stable, but a destroyed entity reports `IsAlive() == false`.
- `registry.IsAlive(id)` / `registry.TryGetEntity(id)` are the checked
  resolvers.
- `Component::GetOwner()` returns the `Entity*`; `GetOwnerId()` returns the
  persistent id.

## Storage

- Data components: a sparse set per type — `vector<T>` values, parallel
  `vector<Entity*>` owners, and a `vector<uint32_t>` sparse index keyed by
  entity index (`0` = absent, otherwise row + 1). Lookup is O(1), iteration is
  insertion order (deterministic), and removal is a swap-delete with a sparse
  fixup.
- Object components: `vector<unique_ptr<T>>`, so the object's address is stable
  across column growth and removal.

## Registry API

```cpp
// entities
Entity& CreateEntity();
void DestroyEntity(Entity&);            // deferred inside ForEach
void DestroyEntity(EntityId);
bool IsAlive(EntityId) const;
std::size_t EntityCount() const;
Entity* TryGetEntity(EntityId);
template<typename Fn> void ForEachEntity(Fn&&);      // (Entity&)

// components
template<typename T, typename... Args> T& AddComponent(Entity&, Args&&...);
template<typename T> void RemoveComponent(Entity&);  // deferred inside ForEach
template<typename T> T* GetComponent(Entity&);
template<typename T> const T* GetComponent(const Entity&) const;
template<typename T> bool HasComponent(const Entity&) const;   // mask, O(1)
template<typename T> std::size_t Count() const;

// queries
template<typename... Ts, typename Fn> void ForEach(Fn&&);       // (Entity&, Ts&...) or (Ts&...)

// engine-owned resource hooks
template<typename T, typename Fn> void OnAdd(Fn&&);    // (Entity&, T&)
template<typename T, typename Fn> void OnRemove(Fn&&); // (Entity&, T&)

// object behavior / structural queue
void UpdateAllComponentsAsync(float delta_time);
void ApplyStructuralChanges();
void Clear();
```

`AddComponent` throws `std::logic_error` on a duplicate, on a destroyed entity,
and when the component-type budget (`kMaxComponentTypes`, 64) is exceeded.

## Queries

`ForEach<Ts...>` iterates the smallest matching pool, tests the entity mask for
the remaining types, then resolves each component. An empty intersection costs
nothing and the visit order is the driving pool's dense order, so gather output
is deterministic. The callback may take the entity as well as the components.

## Structural changes and deferral

`ForEach` holds the registry mutex and increments an iteration depth.
`RemoveComponent` and `DestroyEntity` called from inside a callback are queued
and revalidated when `ApplyStructuralChanges()` runs at the phase boundary (a
destroy already applied makes later queued actions for the same entity no-ops).

`CreateEntity` and `AddComponent` are **immediate** and assert if called during
iteration: their return reference would be meaningless if deferred. Systems that
spawn while iterating collect what they need, then spawn after the loop.

## Engine-owned resources

Data components hold handles, not owners. `DynamicMesh` stores a
`MeshManager::Handle`, and `GameEngine::Setup` registers
`OnRemove<DynamicMesh>` to call `MeshManager::Remove`, which already defers the
GPU free. This keeps engine services out of component types and puts teardown in
one place.

## Threading

Structural changes and queries are single-threaded and never overlap.
`GetComponent` / `HasComponent` are lock-free reads; the mutating APIs, `Count`
and `ForEach` take the registry mutex. `UpdateAllComponentsAsync` collects
object-component pointers under the lock and runs `Update` in parallel over
stable addresses. No structural change may occur while a query or update is in
flight.

## Why not archetypes

Views over per-type sparse sets give deterministic order, O(1) membership and
cheap removal without an archetype graph or move-on-structural-change. The
`ForEach<Ts...>` API is the seam: if profiling ever demands whole-table
iteration, archetype storage can replace the pools behind it without touching
call sites.
