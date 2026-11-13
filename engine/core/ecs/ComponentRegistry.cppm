// Hybrid ECS: object components and data components in one registry.
//
// The contract, in one place:
//
//   * A type that derives from `Component` is an OBJECT component. It is stored
//     as a stable heap object, may hold resources and virtual behavior, and a
//     `T*` to it stays valid for the entity's life.
//   * Any other type is a DATA component. It is stored in a dense contiguous
//     column and addressed through a sparse set. A `T*`/`T&` to it is
//     TRANSIENT: valid until the next structural change (add/remove/destroy).
//     Store an EntityId and re-fetch instead of caching the pointer.
//
// Both kinds live on the same Entity and are queried through the same
// registry; only storage and addressability differ.
//
// Identity: `EntityId` is the persistent handle (generation-checked across slot
// reuse). `Entity&` is valid until the next structural change.
//
// Threading: structural changes and queries are single-threaded and never
// overlap with an update. Object components do not run in one flat fan-out any
// more: every component type declares what it may access (ComponentAccess), the
// declarations resolve into an ordered graph of levels (ComponentSchedule), and
// `UpdateComponents` walks that graph. While a pass is active (`update_active_`)
// EVERY registry entry point throws on EVERY thread — the mutators because they
// would mutate pools a worker is reading, the read-only ones because they hand
// out `Entity&`, whose `GetComponent` yields mutable component pointers. The
// only registry path during a pass is the `UpdateScope` the executor constructs;
// its constructor is private, so the capability cannot be forged. Registration
// is single-threaded by contract: no query may race a first-use registration.
//
// Structural changes: `RemoveComponent` and `DestroyEntity` called from inside
// `ForEach` are deferred and applied by `ApplyStructuralChanges()`. `CreateEntity`
// and `AddComponent` throw during iteration — collect what you want to spawn,
// then spawn after the loop.
//
// Registration is a phase: a type becomes a schedule node at its first use (or
// an explicit `RegisterComponent<T>()`), and the schedule is frozen at the first
// call that needs it. A type that registered after the freeze throws, naming
// itself: this is what lets the graph be resolved once and then read.

module;

#include <cassert>

// logging_macros.hpp has no heavy include, safe in the GMF.
#include <logging/logging_macros.hpp>

export module VulkanEngine.ECS.ComponentRegistry;

import std;
import std.compat;

import logiface;

export import VulkanEngine.ECS.Entity;
export import VulkanEngine.ECS.ComponentAccess;
export import VulkanEngine.ECS.ComponentSchedule;
export import VulkanEngine.ECS.ComponentContext;

import VulkanShared.Jobs;
import VulkanShared.SystemProfile;

export namespace VulkanEngine {

class Entity;
class ComponentRegistry;
class UpdateScope;
class ConfigureGuard;

// ── Component base (object components only) ──
//
// Data components are plain structs and do not derive from this. Keep behavior
// and resource ownership here; keep bulk data out of it.
//
// The nested declaration members are the conservative default: a type that
// declares nothing is `Concurrency::Exclusive` and instance-serial, so the
// engine is safe with no annotations.
class Component {
public:
    virtual ~Component() = default;

    // Called once, immediately after the component is attached and its owner is
    // set. Replaces the old lazy Initialize().
    virtual void OnAttach(Entity& /*owner*/) {}
    // Called once per instance, serially, at the start of the first update pass
    // that sees it - after the instance collection and before any level runs.
    // This is the place for a one-time service binding: it is serial by
    // construction, so a service write here cannot race a level, and it is
    // outside the barrier graph on purpose (the declaration describes Update).
    virtual void OnConfigure(const ComponentContext& /*context*/) {}
    // Called immediately before the component is removed or its entity is
    // destroyed, while the component is still intact.
    virtual void OnDetach(Entity& /*owner*/) {}
    // Per-step behavior for object components. Data components are driven by
    // systems instead. `scope` is the only registry path during an update: it
    // carries the entity identity, the declared-access-checked component
    // lookups, the granted services and the structural queue.
    virtual void Update(float /*delta_time*/, UpdateScope& /*scope*/) {}

    void SetOwner(Entity* entity) noexcept { owner_ = entity; }
    // Identity only: a component may read who it belongs to, but the entity is
    // not a registry handle during an update. Use the scope to reach components.
    [[nodiscard]] const Entity* GetOwner() const noexcept { return owner_; }
    [[nodiscard]] EntityId GetOwnerId() const noexcept;

    using SelfReads = AccessSet<>;
    using SelfWrites = AccessSet<>;
    using Any = AccessSet<>;
    using Before = AccessSet<>;
    using After = AccessSet<>;
    static constexpr Concurrency kConcurrency = Concurrency::Exclusive;
    static constexpr std::uint32_t kServices = 0;
    static constexpr std::string_view kName{};

private:
    friend class ComponentRegistry;

    [[nodiscard]] bool IsConfigured() const noexcept { return configured_; }
    void MarkConfigured() noexcept { configured_ = true; }

    Entity* owner_ = nullptr;
    bool configured_ = false;
};

// ── Entity ──
class Entity {
public:
    Entity() = default;

    [[nodiscard]] EntityId GetId() const noexcept { return id_; }
    [[nodiscard]] bool IsAlive() const noexcept { return alive_; }
    [[nodiscard]] ComponentRegistry& GetRegistry() const noexcept { return *registry_; }
    [[nodiscard]] ComponentRegistry* GetRegistryPtr() const noexcept { return registry_; }

    template<typename T>
    [[nodiscard]] T* GetComponent();
    template<typename T>
    [[nodiscard]] const T* GetComponent() const;

private:
    friend class ComponentRegistry;

    void Activate(std::uint32_t index, ComponentRegistry* registry) noexcept {
        id_ = EntityId{index, generation_};
        registry_ = registry;
        alive_ = true;
        // Slot cleanliness rests entirely on DestroyNow removing every pool row
        // before pushing the index back onto free_slots_.
    }

    void MarkDead() noexcept {
        alive_ = false;
        if (++generation_ == 0) generation_ = 1; // never reuse generation 0
    }

    EntityId id_{};
    ComponentRegistry* registry_ = nullptr;
    bool alive_ = false;
    std::uint32_t generation_ = 1;
};

// ── Component pools ──
class IComponentPool {
public:
    virtual ~IComponentPool() = default;

    [[nodiscard]] virtual std::size_t TypeId() const = 0;
    [[nodiscard]] virtual bool Has(const Entity& entity) const = 0;
    virtual bool Remove(Entity& entity) = 0;
    virtual void Clear() = 0;
    [[nodiscard]] virtual std::size_t Count() const = 0;
    virtual void CollectUpdatables(std::vector<Component*>& out) = 0;
    virtual void ForEachEntity(const std::function<void(Entity&)>& fn) = 0;
};

// Dense column for data components, stable heap objects for object components.
template<typename T>
class ComponentPool final : public IComponentPool {
public:
    static constexpr bool kObject = std::is_base_of_v<Component, T>;

    template<typename... Args>
    T& Emplace(Entity& owner, Args&&... args) {
        if (Has(owner)) {
            throw std::logic_error("Entity already owns this component type");
        }
        const std::uint32_t index = owner.GetId().index;
        EnsureSparse(index);

        if constexpr (kObject) {
            values_.push_back(std::make_unique<T>(std::forward<Args>(args)...));
            values_.back()->SetOwner(&owner);
        } else {
            values_.emplace_back(std::forward<Args>(args)...);
        }
        owners_.push_back(&owner);
        sparse_[index] = static_cast<std::uint32_t>(values_.size()); // row 0 -> 1

        T& component = *At(values_.size() - 1);
        if constexpr (kObject) {
            component.OnAttach(owner);
        } else if constexpr (requires(T& value, Entity& entity) { value.OnAttach(entity); }) {
            component.OnAttach(owner);
        }
        for (auto& hook : on_add_) hook(owner, component);
        return component;
    }

    [[nodiscard]] T* Find(const Entity& entity) {
        const std::uint32_t row = FindRow(entity);
        return row == kNoRow ? nullptr : At(row);
    }
    [[nodiscard]] const T* Find(const Entity& entity) const {
        const std::uint32_t row = FindRow(entity);
        return row == kNoRow ? nullptr : At(row);
    }

    [[nodiscard]] std::size_t TypeId() const override {
        return ComponentTypeIDSystem::GetTypeID<T>();
    }

    [[nodiscard]] bool Has(const Entity& entity) const override {
        return FindRow(entity) != kNoRow;
    }

    bool Remove(Entity& entity) override {
        const std::uint32_t row = FindRow(entity);
        if (row == kNoRow) return false;

        T& component = *At(row);
        if constexpr (kObject) {
            component.OnDetach(entity);
        } else if constexpr (requires(T& value, Entity& owner) { value.OnDetach(owner); }) {
            component.OnDetach(entity);
        }
        for (auto& hook : on_remove_) hook(entity, component);

        // Swap-delete: the moved-into-row owner gets its sparse index fixed up.
        const std::size_t last = values_.size() - 1;
        if (row != last) {
            std::swap(values_[row], values_[last]);
            std::swap(owners_[row], owners_[last]);
            sparse_[owners_[row]->GetId().index] = row + 1;
        }
        values_.pop_back();
        owners_.pop_back();
        sparse_[entity.GetId().index] = 0;
        return true;
    }

    void Clear() override {
        for (std::size_t i = 0; i < values_.size(); ++i) {
            T& component = *At(i);
            Entity& owner = *owners_[i];
            if constexpr (kObject) {
                component.OnDetach(owner);
            } else if constexpr (requires(T& value, Entity& entity) { value.OnDetach(entity); }) {
                component.OnDetach(owner);
            }
            for (auto& hook : on_remove_) hook(owner, component);
        }
        values_.clear();
        owners_.clear();
        sparse_.clear();
    }

    [[nodiscard]] std::size_t Count() const override { return values_.size(); }

    void CollectUpdatables(std::vector<Component*>& out) override {
        if constexpr (kObject) {
            for (auto& value : values_) out.push_back(value.get());
        }
    }

    void ForEachEntity(const std::function<void(Entity&)>& fn) override {
        for (Entity* owner : owners_) fn(*owner);
    }

    void AddOnAdd(std::function<void(Entity&, T&)> hook) { on_add_.push_back(std::move(hook)); }
    void AddOnRemove(std::function<void(Entity&, T&)> hook) { on_remove_.push_back(std::move(hook)); }

private:
    static constexpr std::uint32_t kNoRow = std::numeric_limits<std::uint32_t>::max();

    using Storage = std::conditional_t<kObject, std::vector<std::unique_ptr<T>>, std::vector<T>>;

    [[nodiscard]] T* At(std::size_t row) {
        if constexpr (kObject) return values_[row].get();
        else return &values_[row];
    }
    [[nodiscard]] const T* At(std::size_t row) const {
        if constexpr (kObject) return values_[row].get();
        else return &values_[row];
    }

    [[nodiscard]] std::uint32_t FindRow(const Entity& entity) const {
        const std::uint32_t index = entity.GetId().index;
        if (index >= sparse_.size()) return kNoRow;
        const std::uint32_t encoded = sparse_[index];
        return encoded == 0 ? kNoRow : encoded - 1;
    }

    void EnsureSparse(std::uint32_t index) {
        if (sparse_.size() <= index) sparse_.resize(static_cast<std::size_t>(index) + 1, 0);
    }

    Storage values_{};
    std::vector<Entity*> owners_{};
    std::vector<std::uint32_t> sparse_{};
    std::vector<std::function<void(Entity&, T&)>> on_add_{};
    std::vector<std::function<void(Entity&, T&)>> on_remove_{};
};

// ── Registry ──
class ComponentRegistry : public IViolationSink {
public:
    // `scheduler` is nullable on purpose. A reference default would start the
    // process-wide pool at EngineContext construction, before EngineBootstrap
    // sizes it, and every device-free test that declares a registry would spawn
    // workers it never uses. Tests pass a local pool to prove overlap.
    explicit ComponentRegistry(VulkanShared::JobScheduler* scheduler = nullptr)
        : scheduler_(scheduler) {}

    ~ComponentRegistry() { ClearNoThrow(); }

    ComponentRegistry(const ComponentRegistry&) = delete;
    ComponentRegistry& operator=(const ComponentRegistry&) = delete;

    // ── Entities ──
    Entity& CreateEntity() {
        // Pre-lock: `ForEach` holds the non-recursive mutex_ across its
        // callback, so a post-lock depth test from inside one would block
        // instead of throwing.
        ThrowIfIterating("CreateEntity");
        const std::scoped_lock lock(mutex_);
        ThrowIfUpdating("CreateEntity");
        std::uint32_t index = 0;
        if (!free_slots_.empty()) {
            index = free_slots_.back();
            free_slots_.pop_back();
        } else {
            index = static_cast<std::uint32_t>(slots_.size());
            slots_.push_back(std::make_unique<Entity>());
        }
        Entity& entity = *slots_[index];
        entity.Activate(index, this);
        // A recycled slot must be clean. Presence now lives only in the pools,
        // so a row left behind by a previous occupant would make the reused
        // entity report HasComponent and hand out the previous entity's data.
#ifndef NDEBUG
        for (const auto& slot : types_) {
            assert(slot == nullptr || !slot->pool->Has(entity));
        }
#endif
        return entity;
    }

    void DestroyEntity(Entity& entity) {
        ThrowIfCalledFromComponent("DestroyEntity");
        // The unlocked reads are only an early-out; the authoritative check is
        // the one under mutex_ below.
        if (update_active_.load(std::memory_order_relaxed) ||
            iteration_depth_.load(std::memory_order_relaxed) > 0) {
            QueueDestroy(entity.GetId());
            return;
        }
        const std::scoped_lock lock(mutex_);
        if (update_active_.load(std::memory_order_relaxed)) {
            QueueDestroy(entity.GetId());
            return;
        }
        DestroyNow(entity);
    }

    void DestroyEntity(EntityId id) {
        ThrowIfCalledFromComponent("DestroyEntity");
        if (update_active_.load(std::memory_order_relaxed) ||
            iteration_depth_.load(std::memory_order_relaxed) > 0) {
            QueueDestroy(id);
            return;
        }
        // Resolve through the private unchecked lookup: the public TryGetEntity
        // throws during a pass, and this overload obeys the same rule as the
        // reference one.
        const std::scoped_lock lock(mutex_);
        if (update_active_.load(std::memory_order_relaxed)) {
            QueueDestroy(id);
            return;
        }
        if (Entity* entity = UncheckedEntity(id)) DestroyNow(*entity);
    }

    [[nodiscard]] bool IsAlive(EntityId id) const {
        ThrowIfUpdating("ComponentRegistry::IsAlive");
        return UncheckedEntity(id) != nullptr;
    }

    [[nodiscard]] std::size_t EntityCount() const {
        ThrowIfUpdating("ComponentRegistry::EntityCount");
        const std::scoped_lock lock(mutex_);
        // Authoritative: the pre-lock call above is only an early-out.
        ThrowIfUpdating("ComponentRegistry::EntityCount");
        return slots_.size() - free_slots_.size();
    }

    [[nodiscard]] Entity* TryGetEntity(EntityId id) {
        ThrowIfUpdating("ComponentRegistry::TryGetEntity");
        return UncheckedEntity(id);
    }

    template<typename Fn>
    void ForEachEntity(Fn&& fn) {
        ThrowIfUpdating("ComponentRegistry::ForEachEntity");
        const std::scoped_lock lock(mutex_);
        // Authoritative: the pre-lock call above is only an early-out.
        ThrowIfUpdating("ComponentRegistry::ForEachEntity");
        for (auto& slot : slots_) {
            if (slot != nullptr && slot->alive_) fn(*slot);
        }
    }

    // ── Components ──
    template<typename T, typename... Args>
    T& AddComponent(Entity& owner, Args&&... args) {
        ThrowIfIterating("AddComponent");
        const std::scoped_lock lock(mutex_);
        ThrowIfUpdating("AddComponent");
        if (!owner.alive_) {
            throw std::logic_error("AddComponent on a destroyed entity");
        }
        auto& pool = GetOrCreatePool<T>();
        if constexpr (ComponentPool<T>::kObject) {
            // Cleared BEFORE Emplace: Emplace runs OnAttach and the OnAdd hooks,
            // and a hook that throws deliberately leaves the row visible (see
            // below), so that instance still has to be configured.
            all_configured_ = false;
        }
        T& component = pool.Emplace(owner, std::forward<Args>(args)...);
        // Emplace writes sparse_ before running OnAttach and the OnAdd hooks, so
        // a hook that throws leaves the component visible to HasComponent,
        // GetComponent, Count and ForEach. That is deliberate: a throwing hook
        // must not leave an invisible-but-counted row behind.
        return component;
    }

    template<typename T>
    void RemoveComponent(Entity& entity) {
        ThrowIfCalledFromComponent("RemoveComponent");
        if (update_active_.load(std::memory_order_relaxed) ||
            iteration_depth_.load(std::memory_order_relaxed) > 0) {
            QueueRemove<T>(entity.GetId());
            return;
        }
        const std::scoped_lock lock(mutex_);
        if (update_active_.load(std::memory_order_relaxed)) {
            QueueRemove<T>(entity.GetId());
            return;
        }
        RemoveInternal<T>(entity);
    }

    template<typename T>
    [[nodiscard]] T* GetComponent(Entity& entity) {
        ThrowIfUpdating("ComponentRegistry::GetComponent");
        return UncheckedGet<T>(entity);
    }

    template<typename T>
    [[nodiscard]] const T* GetComponent(const Entity& entity) const {
        ThrowIfUpdating("ComponentRegistry::GetComponent");
        return UncheckedGet<T>(entity);
    }

    template<typename T>
    [[nodiscard]] bool HasComponent(const Entity& entity) const {
        ThrowIfUpdating("ComponentRegistry::HasComponent");
        const auto* pool = GetPool<T>();
        return pool != nullptr && pool->Has(entity);
    }

    template<typename T>
    [[nodiscard]] std::size_t Count() const {
        ThrowIfUpdating("ComponentRegistry::Count");
        const std::scoped_lock lock(mutex_);
        // Authoritative: the pre-lock call above is only an early-out.
        ThrowIfUpdating("ComponentRegistry::Count");
        const auto* pool = GetPool<T>();
        return pool != nullptr ? pool->Count() : 0;
    }

    // ── Queries ──
    //
    // Visits every entity that has all of Ts, iterating the smallest matching
    // pool so empty intersections cost nothing. Each component is resolved once
    // per entity and the same pointer is the membership test, so a query costs
    // one sparse lookup per requested type. `fn` may take (Entity&, Ts&...) or
    // (Ts&...). Order is the driving pool's insertion order (deterministic).
    template<typename... Ts, typename Fn>
    void ForEach(Fn&& fn) {
        static_assert(sizeof...(Ts) > 0, "ForEach requires at least one component type");
        ThrowIfUpdating("ComponentRegistry::ForEach");
        const std::scoped_lock lock(mutex_);
        // Authoritative: the pre-lock call above is only an early-out.
        ThrowIfUpdating("ComponentRegistry::ForEach");
        ++iteration_depth_;
        struct DepthGuard {
            std::atomic<std::size_t>& depth;
            ~DepthGuard() { --depth; }
        } guard{iteration_depth_};

        ForEachDriver<Ts...>([&](Entity& entity, Ts&... components) {
            if constexpr (std::is_invocable_v<Fn&, Entity&, Ts&...>) {
                std::invoke(fn, entity, components...);
            } else {
                std::invoke(fn, components...);
            }
        });
    }

    // ── Engine-owned resource hooks ──
    template<typename T, typename Fn>
    void OnAdd(Fn&& fn) {
        const std::scoped_lock lock(mutex_);
        ThrowIfUpdating("ComponentRegistry::OnAdd");
        ThrowIfUpdating("ComponentRegistry::OnAdd");
        GetOrCreatePool<T>().AddOnAdd(std::forward<Fn>(fn));
    }

    template<typename T, typename Fn>
    void OnRemove(Fn&& fn) {
        const std::scoped_lock lock(mutex_);
        ThrowIfUpdating("ComponentRegistry::OnRemove");
        ThrowIfUpdating("ComponentRegistry::OnRemove");
        GetOrCreatePool<T>().AddOnRemove(std::forward<Fn>(fn));
    }

    // ── Registration ──
    //
    // A component type becomes a graph node at its first use, before the
    // schedule is frozen. Read-only use (GetComponent/HasComponent/Count) never
    // registers anything: a lookup cannot create a node.
    template<typename T>
    void RegisterComponent() {
        static_assert(ComponentPool<T>::kObject,
                      "only object components are schedule nodes; a data component has no Update");
        const std::scoped_lock lock(mutex_);
        ThrowIfUpdating("ComponentRegistry::RegisterComponent");
        ThrowIfUpdating("ComponentRegistry::RegisterComponent");
        // Creates the slot/pool and registers the node: a type can be a node
        // before it has any instance.
        (void)GetOrCreatePool<T>();
    }

    // Forces the close and returns the frozen schedule. Idempotent. The correct
    // moment is after the application registered its component types - at the
    // end of its setup hook, or just before its first update. It must NOT be
    // called from EngineBootstrap::Initialize: bootstrap runs before app setup,
    // so closing there resolves a zero-node schedule and every later
    // AddComponent throws.
    [[nodiscard]] const ComponentSchedule& CloseRegistration() {
        const std::scoped_lock lock(mutex_);
        ThrowIfUpdating("ComponentRegistry::CloseRegistration");
        ThrowIfUpdating("ComponentRegistry::CloseRegistration");
        return EnsureClosedLocked();
    }

    // ── Object-component behavior ──
    void UpdateComponents(float delta_time, const ComponentContext& context = {},
                          VulkanShared::ISystemProfileSink* profile = nullptr);

    // ── Schedule views ──
    [[nodiscard]] const ComponentSchedule& Schedule() {
        const std::scoped_lock lock(mutex_);
        ThrowIfUpdating("ComponentRegistry::Schedule");
        ThrowIfUpdating("ComponentRegistry::Schedule");
        return EnsureClosedLocked();
    }

    [[nodiscard]] const ComponentAccessInfo* AccessInfo(std::size_t type_id) {
        const std::scoped_lock lock(mutex_);
        ThrowIfUpdating("ComponentRegistry::AccessInfo");
        ThrowIfUpdating("ComponentRegistry::AccessInfo");
        (void)EnsureClosedLocked();
        if (type_id >= types_.size() || types_[type_id] == nullptr) return nullptr;
        const std::size_t index = types_[type_id]->info_index;
        return index == kNoInfo ? nullptr : &infos_[index];
    }

    [[nodiscard]] NameTable TargetNames() {
        const std::scoped_lock lock(mutex_);
        ThrowIfUpdating("ComponentRegistry::TargetNames");
        ThrowIfUpdating("ComponentRegistry::TargetNames");
        (void)EnsureClosedLocked();
        return target_names_;
    }

    // Between steps only: the plan reflects live instance counts, so it is not
    // available while a pass is running. The executor computes its own plan
    // before raising the guard.
    [[nodiscard]] std::vector<PlanSegment> ExecutionPlan() {
        const std::scoped_lock lock(mutex_);
        ThrowIfUpdating("ComponentRegistry::ExecutionPlan");
        ThrowIfUpdating("ComponentRegistry::ExecutionPlan");
        const ComponentSchedule& schedule = EnsureClosedLocked();
        CollectLocked(schedule);
        return schedule.ExecutionPlan(unit_counts_);
    }

    [[nodiscard]] std::string Describe() {
        const std::scoped_lock lock(mutex_);
        ThrowIfUpdating("ComponentRegistry::Describe");
        ThrowIfUpdating("ComponentRegistry::Describe");
        return EnsureClosedLocked().Describe();
    }

    void SetAccessOptions(ComponentAccessOptions options) {
        const std::scoped_lock lock(mutex_);
        ThrowIfUpdating("ComponentRegistry::SetAccessOptions");
        ThrowIfUpdating("ComponentRegistry::SetAccessOptions");
        access_options_ = options;
    }

    [[nodiscard]] ComponentAccessOptions AccessOptions() const {
        const std::scoped_lock lock(mutex_);
        return access_options_;
    }

    // ── Structural queue ──
    void ApplyStructuralChanges() {
        const std::scoped_lock lock(mutex_);
        // The check must be inside the lock: a pre-lock check would leave the
        // window where it passes and the update then starts, letting
        // DestroyNow/RemoveInternal run while the pass holds live pointers.
        ThrowIfUpdating("ComponentRegistry::ApplyStructuralChanges");
        std::vector<std::function<void()>> pending;
        {
            const std::scoped_lock pending_lock(pending_mutex_);
            pending.swap(pending_);
        }
        // Applied while still holding mutex_, so today's guarantee holds: a
        // drain can never overlap a pass.
        for (auto& action : pending) action();
    }

    // Drops entities, pools and the pending queue, and KEEPS the registrations
    // and the resolved schedule: a declaration is a property of the type, not of
    // the entity population.
    void Clear() {
        const std::scoped_lock lock(mutex_);
        ThrowIfUpdating("ComponentRegistry::Clear");
        ClearCore();
    }

private:
    friend class UpdateScope;
    friend class ConfigureGuard;

    static constexpr std::size_t kNoInfo = std::numeric_limits<std::size_t>::max();

    struct TypeSlot {
        std::unique_ptr<IComponentPool> pool;
        std::size_t info_index = kNoInfo;
        std::size_t discovery_index = 0;
        const ComponentAccessSpec* spec = nullptr;
        bool object = false;
    };

    // One dispatch element: either a single instance (a SelfParallel type) or
    // one whole type's instance range run serially inside the unit.
    struct Unit {
        std::size_t discovery = 0;
        std::size_t first = 0;
        std::size_t count = 0;
    };

    // Raises the flag for the duration of a pass and clears it on every exit,
    // including an exception from Update. Only clears it if this guard raised it,
    // so a rejected re-entrant call cannot unset the outer pass's flag.
    class UpdateActiveGuard {
    public:
        explicit UpdateActiveGuard(std::atomic<bool>& flag) noexcept : flag_(&flag) {}
        UpdateActiveGuard(const UpdateActiveGuard&) = delete;
        UpdateActiveGuard& operator=(const UpdateActiveGuard&) = delete;

        void Activate() noexcept {
            flag_->store(true, std::memory_order_release);
            activated_ = true;
        }
        ~UpdateActiveGuard() {
            if (activated_) flag_->store(false, std::memory_order_release);
        }

    private:
        std::atomic<bool>* flag_;
        bool activated_ = false;
    };

    struct ViolationKey {
        ViolationKind kind = ViolationKind::UndeclaredSelfRead;
        std::size_t type_id = 0;
        std::size_t target_id = 0;
        bool service = false;

        friend bool operator==(const ViolationKey&, const ViolationKey&) = default;
    };

    struct ViolationKeyHash {
        std::size_t operator()(const ViolationKey& key) const noexcept {
            std::size_t seed = std::hash<std::size_t>{}(key.type_id);
            const auto mix = [&seed](std::size_t value) {
                seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
            };
            mix(std::to_underlying(key.kind));
            mix(key.target_id);
            mix(key.service ? 1u : 0u);
            return seed;
        }
    };

    // ── Guards ──

    // Two-phase rejection: the call before a lock acquisition is an early-out,
    // the call under `mutex_` is the authoritative one. A thread that passes the
    // first can block on the mutex and acquire it after a pass has started, so
    // every member that takes `mutex_` tests again while holding it.
    void ThrowIfUpdating(const char* what) const {
        if (update_active_.load(std::memory_order_relaxed)) {
            throw std::logic_error(std::format(
                "{} is not allowed during a component update: the update pass owns the "
                "registry. Use the UpdateScope the component received.",
                what));
        }
    }

    void ThrowIfIterating(const char* what) const {
        if (iteration_depth_.load(std::memory_order_relaxed) > 0) {
            throw std::logic_error(std::format(
                "{} is not allowed during ForEach: collect what you want to spawn, then "
                "spawn after the loop.",
                what));
        }
    }

    void ThrowIfCalledFromComponent(const char* what) const {
        if (CurrentAccessMode() != AccessMode::None) {
            throw std::logic_error(std::format(
                "{} is not allowed from a component: use the UpdateScope it received.", what));
        }
    }

    // ── Pools and slots ──

    [[nodiscard]] VulkanShared::JobScheduler& Scheduler() {
        return scheduler_ != nullptr ? *scheduler_ : VulkanShared::JobScheduler::Global();
    }

    template<typename T>
    [[nodiscard]] ComponentPool<T>* GetPool() {
        const std::size_t type_id = ComponentTypeIDSystem::GetTypeID<T>();
        if (type_id >= types_.size() || types_[type_id] == nullptr) return nullptr;
        return static_cast<ComponentPool<T>*>(types_[type_id]->pool.get());
    }

    template<typename T>
    [[nodiscard]] const ComponentPool<T>* GetPool() const {
        const std::size_t type_id = ComponentTypeIDSystem::GetTypeID<T>();
        if (type_id >= types_.size() || types_[type_id] == nullptr) return nullptr;
        return static_cast<const ComponentPool<T>*>(types_[type_id]->pool.get());
    }

    template<typename T>
    ComponentPool<T>& GetOrCreatePool() {
        const std::size_t type_id = ComponentTypeIDSystem::GetTypeID<T>();
        if (type_id >= types_.size()) types_.resize(type_id + 1);
        if (types_[type_id] == nullptr) {
            auto slot = std::make_unique<TypeSlot>();
            slot->object = ComponentPool<T>::kObject;
            slot->pool = std::make_unique<ComponentPool<T>>();
            types_[type_id] = std::move(slot);
        }
        if constexpr (ComponentPool<T>::kObject) {
            RegisterTypeLocked<T>(type_id);
        }
        return *static_cast<ComponentPool<T>*>(types_[type_id]->pool.get());
    }

    template<typename T>
    void RegisterTypeLocked(std::size_t type_id) {
        // Already a node: re-adding an instance of a registered type is always
        // allowed, including after the freeze.
        if (type_id < types_.size() && types_[type_id] != nullptr &&
            types_[type_id]->info_index != kNoInfo) {
            return;
        }
        if (closed_) {
            // `TypeName` is consteval, so it must be folded at compile time; a
            // call in the middle of a runtime format expression is not portable.
            constexpr std::string_view kTypeName = TypeName<T>();
            throw std::logic_error(std::format(
                "cannot register component type '{}' after registration closed: the component "
                "schedule is frozen. Call RegisterComponent<{}>() before the first update.",
                kAccessSpec<T>.name, kTypeName));
        }
        assert(type_id < types_.size() && types_[type_id] != nullptr &&
               "GetOrCreatePool must create the slot before registering the type");
        auto& slot = *types_[type_id];
        slot.info_index = infos_.size();
        slot.discovery_index = infos_.size();
        slot.spec = &kAccessSpec<T>;
        ComponentAccessInfo info;
        info.type_id = type_id;
        info.discovery_index = slot.discovery_index;
        infos_.push_back(std::move(info));
        specs_.push_back(slot.spec);
    }

    [[nodiscard]] std::size_t DiscoveryOfLocked(std::size_t type_id) const noexcept {
        if (type_id >= types_.size() || types_[type_id] == nullptr) return kNoInfo;
        return types_[type_id]->info_index == kNoInfo ? kNoInfo : types_[type_id]->discovery_index;
    }

    // ── Unchecked query path (UpdateScope and the drain only) ──

    [[nodiscard]] Entity* UncheckedEntity(EntityId id) const {
        if (id.index >= slots_.size()) return nullptr;
        Entity& entity = *slots_[id.index];
        if (!entity.alive_ || entity.id_.generation != id.generation) return nullptr;
        return &entity;
    }

    template<typename T>
    [[nodiscard]] T* UncheckedGet(Entity& entity) {
        auto* pool = GetPool<T>();
        return pool != nullptr ? pool->Find(entity) : nullptr;
    }

    template<typename T>
    [[nodiscard]] const T* UncheckedGet(const Entity& entity) const {
        const auto* pool = GetPool<T>();
        return pool != nullptr ? pool->Find(entity) : nullptr;
    }

    // The shared body of the public ForEach and the scope's ForEach: the caller
    // decides which identity the callback receives.
    template<typename... Ts, typename Visit>
    void ForEachDriver(Visit&& visit) {
        const std::array<IComponentPool*, sizeof...(Ts)> candidates{
            static_cast<IComponentPool*>(GetPool<Ts>())...};

        IComponentPool* driver = nullptr;
        for (IComponentPool* candidate : candidates) {
            if (candidate == nullptr) continue;
            if (driver == nullptr || candidate->Count() < driver->Count()) driver = candidate;
        }
        if (driver == nullptr || driver->Count() == 0) return;

        driver->ForEachEntity([&](Entity& entity) {
            // Resolve each component once: membership is the pool's sparse
            // lookup, so test and fetch must not look it up twice. Folding over
            // the tuple rather than std::get<T> keeps ForEach<T, T> well-formed.
            auto members = std::make_tuple(UncheckedGet<Ts>(entity)...);
            const bool has_all =
                std::apply([](auto*... p) { return (... && (p != nullptr)); }, members);
            if (!has_all) return;
            std::apply([&](auto*... p) { visit(entity, *p...); }, members);
        });
    }

    // ── Structural operations (callers hold mutex_) ──

    void DestroyNow(Entity& entity) {
        if (!entity.alive_) return;
        for (auto& slot : types_) {
            if (slot != nullptr) slot->pool->Remove(entity);
        }
        entity.MarkDead();
        free_slots_.push_back(entity.id_.index);
    }

    template<typename T>
    void RemoveInternal(Entity& entity) {
        if (auto* pool = GetPool<T>()) pool->Remove(entity);
    }

    void QueueDestroy(EntityId id) {
        const std::scoped_lock lock(pending_mutex_);
        pending_.push_back([this, id] {
            if (Entity* target = UncheckedEntity(id)) DestroyNow(*target);
        });
    }

    template<typename T>
    void QueueRemove(EntityId id) {
        const std::scoped_lock lock(pending_mutex_);
        pending_.push_back([this, id] {
            if (Entity* target = UncheckedEntity(id)) RemoveInternal<T>(*target);
        });
    }

    void ClearCore() {
        {
            const std::scoped_lock pending_lock(pending_mutex_);
            pending_.clear();
        }
        for (auto& slot : slots_) {
            if (slot != nullptr && slot->alive_) DestroyNow(*slot);
        }
        slots_.clear();
        free_slots_.clear();
        for (auto& slot : types_) {
            if (slot != nullptr) slot->pool->Clear();
        }
        all_configured_ = true;
    }

    // ~ComponentRegistry is implicitly noexcept; a throw there would terminate.
    void ClearNoThrow() noexcept {
        const std::scoped_lock lock(mutex_);
        try {
            ClearCore();
        } catch (...) {
            // A component's destructor hook threw during teardown. Nothing can
            // propagate from here, and the pools are already being dropped.
        }
    }

    // ── Registration close ──

    void FillInfosLocked() {
        target_names_.clear();
        auto fill_known = [this](std::span<const TargetRef> refs,
                                 std::flat_set<std::size_t>& out) {
            out.clear();
            for (const TargetRef& ref : refs) {
                if (ref.id == nullptr) continue;
                const std::size_t id = ref.id();
                out.insert(id);
                target_names_.push_back({id, ref.name});
            }
        };

        for (std::size_t discovery = 0; discovery < infos_.size(); ++discovery) {
            ComponentAccessInfo& info = infos_[discovery];
            const ComponentAccessSpec& spec = *specs_[discovery];
            info.name = spec.name;
            info.source = spec.source;
            info.concurrency = spec.concurrency;
            info.services = spec.services;

            fill_known(spec.self_reads, info.self_reads);
            fill_known(spec.self_writes, info.self_writes);
            fill_known(spec.any, info.any);

            // Ordering targets that live in no pool are dropped before edge
            // building, so no edge is ever attempted toward a non-node. A target
            // that is not here makes the constraint vacuous; treating it as an
            // error would break libraries whose optional ordering names a type
            // the host did not use.
            auto fill_ordering = [&](std::span<const TargetRef> refs,
                                     std::flat_set<std::size_t>& out) {
                out.clear();
                for (const TargetRef& ref : refs) {
                    if (ref.id == nullptr) continue;
                    const std::size_t id = ref.id();
                    target_names_.push_back({id, ref.name});
                    if (DiscoveryOfLocked(id) == kNoInfo) {
                        LOGIFACE_LOG(warn, std::format(
                            "component '{}' declares ordering towards '{}', which is not "
                            "registered here; the constraint is dropped",
                            info.name, ref.name));
                        continue;
                    }
                    out.insert(id);
                }
            };
            fill_ordering(spec.before, info.before);
            fill_ordering(spec.after, info.after);

            // Every object component implicitly writes itself: its own Update
            // mutates its own members. MakeAccessSpec never adds it; this is the
            // one owner of the insertion.
            info.self_writes.insert(info.type_id);
        }

        std::ranges::sort(target_names_, {}, &NameEntry::type_id);
        const auto unique = std::ranges::unique(target_names_, {}, &NameEntry::type_id);
        target_names_.erase(unique.begin(), unique.end());
    }

    // The single entry point that mutates the frozen state, always under mutex_.
    [[nodiscard]] const ComponentSchedule& EnsureClosedLocked() {
        if (closed_) {
            if (close_error_) throw std::logic_error(close_error_->message);
            return *schedule_;
        }
        closed_ = true;
        FillInfosLocked();
        auto result = ComponentSchedule::Resolve(infos_, target_names_);
        if (!result) {
            close_error_ = std::move(result.error());
            throw std::logic_error(close_error_->message);
        }
        schedule_.emplace(std::move(*result));
        LOGIFACE_LOG(debug, schedule_->Describe());
        return *schedule_;
    }

    // ── Per-step collection and execution ──

    void CollectLocked(const ComponentSchedule& schedule) {
        const std::size_t type_count = infos_.size();
        flat_.clear();
        type_first_.assign(type_count, 0);
        type_count_.assign(type_count, 0);
        unit_counts_.assign(type_count, 0);

        for (std::size_t discovery = 0; discovery < type_count; ++discovery) {
            type_first_[discovery] = flat_.size();
            const std::size_t type_id = infos_[discovery].type_id;
            if (type_id < types_.size() && types_[type_id] != nullptr) {
                types_[type_id]->pool->CollectUpdatables(flat_);
            }
            type_count_[discovery] = flat_.size() - type_first_[discovery];
        }

        units_.clear();
        level_units_.assign(schedule.Levels().size(), {0, 0});
        for (std::size_t level = 0; level < schedule.Levels().size(); ++level) {
            level_units_[level].first = units_.size();
            for (const std::size_t type_id : schedule.Levels()[level].type_ids) {
                const std::size_t discovery = DiscoveryOfLocked(type_id);
                if (discovery == kNoInfo) continue;
                const ComponentAccessInfo& info = infos_[discovery];
                const std::size_t count = type_count_[discovery];
                std::size_t produced = 0;
                if (info.concurrency == Concurrency::SelfParallel) {
                    for (std::size_t row = 0; row < count; ++row) {
                        units_.push_back(Unit{discovery, type_first_[discovery] + row, 1});
                    }
                    produced = count;
                } else if (count != 0) {
                    // One many-instance unit: the type keeps its instances
                    // serial while still overlapping the level's other types.
                    units_.push_back(Unit{discovery, type_first_[discovery], count});
                    produced = 1;
                }
                unit_counts_[discovery] = produced;
            }
            level_units_[level].second = units_.size();
        }
    }

    [[nodiscard]] std::string SegmentLabel(const PlanSegment& segment) const {
        if (segment.parallel) {
            return std::format("component.parallel[{}]", segment.first_level);
        }
        if (segment.level_count == 1) {
            return std::format("component.serial[{}]", segment.first_level);
        }
        return std::format("component.serial[{}-{}]", segment.first_level,
                           segment.first_level + segment.level_count - 1);
    }

    void ConfigurePass(const ComponentContext& context);

    void RunUnit(const Unit& unit, float delta_time, const ComponentContext& context);

    void RunPass(float delta_time, const ComponentContext& context,
                 VulkanShared::ISystemProfileSink* profile);

    // IViolationSink. Private override: reachable only through the interface the
    // TLS record carries, never by name.
    void OnViolation(const Violation& violation) override {
        const AccessRecord* record = CurrentAccess();
        const ComponentAccessOptions options =
            record != nullptr ? record->options : access_options_;
        if (options.on_violation == ViolationAction::Throw) {
            throw std::logic_error(FormatViolation(violation));
        }
        const ViolationKey key{violation.kind, violation.type_id, violation.target_id,
                               violation.service};
        {
            const std::scoped_lock lock(violation_mutex_);
            if (!reported_violations_.insert(key).second) return;
        }
        LOGIFACE_LOG(error, FormatViolation(violation));
    }

    VulkanShared::JobScheduler* scheduler_ = nullptr;

    std::vector<std::unique_ptr<TypeSlot>> types_{};
    std::vector<std::unique_ptr<Entity>> slots_{};
    std::vector<std::uint32_t> free_slots_{};
    std::vector<std::function<void()>> pending_{};
    std::atomic<std::size_t> iteration_depth_{0};
    std::atomic<bool> update_active_{false};
    mutable std::mutex mutex_{};
    mutable std::mutex pending_mutex_{};

    // Contiguous, in registration order, appended only while registration is
    // open and never resized after close: the schedule spans into it.
    std::vector<ComponentAccessInfo> infos_{};
    std::vector<const ComponentAccessSpec*> specs_{};
    std::vector<NameEntry> target_names_{};
    std::optional<ComponentSchedule> schedule_{};
    std::optional<ScheduleError> close_error_{};
    bool closed_ = false;
    // True once every live instance has run OnConfigure. Cleared when an
    // instance is added, so a steady state skips the pass entirely.
    bool all_configured_ = true;

    ComponentAccessOptions access_options_{};
    std::mutex violation_mutex_{};
    std::unordered_set<ViolationKey, ViolationKeyHash> reported_violations_{};

    // Reused across steps so a pass allocates nothing for the plan.
    std::vector<Component*> flat_{};
    std::vector<std::size_t> type_first_{};
    std::vector<std::size_t> type_count_{};
    std::vector<std::size_t> unit_counts_{};
    std::vector<Unit> units_{};
    std::vector<std::pair<std::size_t, std::size_t>> level_units_{};
    std::vector<PlanSegment> plan_{};
};

// ── UpdateScope ──
//
// The only registry path during an update, and an accessor set over the
// declaring type's resolved declaration. The private constructor plus the
// deleted special members are the capability: a public constructor would let any
// code holding a registry forge a scope, and a movable scope could outlive the
// call and leave the TLS slot open. The executor is the only construction site.
class UpdateScope {
public:
    UpdateScope(const UpdateScope&) = delete;
    UpdateScope& operator=(const UpdateScope&) = delete;
    UpdateScope(UpdateScope&&) = delete;
    UpdateScope& operator=(UpdateScope&&) = delete;

    [[nodiscard]] EntityId SelfId() const noexcept {
        const AccessRecord* record = CurrentAccess();
        return record != nullptr ? record->owner : EntityId{};
    }

    // Own entity. Find requires a write declaration; Read accepts read or write.
    template<typename T>
    T* Find() {
        RequireSelf<T>(true);
        Entity* owner = registry_.UncheckedEntity(SelfId());
        return owner != nullptr ? registry_.UncheckedGet<T>(*owner) : nullptr;
    }
    template<typename T>
    const T* Read() {
        RequireSelf<T>(false);
        Entity* owner = registry_.UncheckedEntity(SelfId());
        return owner != nullptr ? registry_.UncheckedGet<T>(*owner) : nullptr;
    }

    // Cross-entity. Both require T in Any<T>.
    template<typename T>
    T* Find(EntityId entity) {
        RequireAny<T>();
        Entity* target = registry_.UncheckedEntity(entity);
        return target != nullptr ? registry_.UncheckedGet<T>(*target) : nullptr;
    }
    template<typename T>
    const T* Read(EntityId entity) {
        RequireAny<T>();
        Entity* target = registry_.UncheckedEntity(entity);
        return target != nullptr ? registry_.UncheckedGet<T>(*target) : nullptr;
    }

    // All matching entities. Requires every T in Any<T>. Accepts both callable
    // shapes ComponentRegistry::ForEach accepts - fn(EntityId, Ts&...) and
    // fn(Ts&...) - so a migrated unary lambda still compiles. The callback
    // receives the visited EntityId, so it can queue a destroy or do a further
    // scoped lookup without a raw Entity*.
    template<typename... Ts, typename Fn>
        requires (std::invocable<Fn&, EntityId, Ts&...> || std::invocable<Fn&, Ts&...>)
    void ForEach(Fn&& fn) {
        (RequireAny<Ts>(), ...);
        registry_.ForEachDriver<Ts...>([&](Entity& entity, Ts&... components) {
            if constexpr (std::invocable<Fn&, EntityId, Ts&...>) {
                std::invoke(fn, entity.GetId(), components...);
            } else {
                std::invoke(fn, components...);
            }
        });
    }

    [[nodiscard]] const ComponentContext& Services() const noexcept { return *context_; }

    void Destroy(EntityId entity) { registry_.QueueDestroy(entity); }
    template<typename T>
    void Remove(EntityId entity) {
        registry_.QueueRemove<T>(entity);
    }

private:
    friend class ComponentRegistry;

    // The executor installs the record for the whole work unit (one type) and
    // this constructor only refreshes the owner: installing the full record per
    // instance would dominate the cost of a trivial Update.
    UpdateScope(ComponentRegistry& registry, Component& component,
                const ComponentAccessInfo& info, const ComponentContext& context)
        : registry_(registry), info_(info), context_(&context) {
        assert(CurrentAccessMode() == AccessMode::Update &&
               "the executor must install the access record before constructing a scope");
        SetCurrentAccessOwner(component.GetOwnerId());
    }

    template<typename T>
    void RequireSelf(bool write) {
        if constexpr (!kAccessChecksCompiled) {
            (void)write;
        } else {
            const AccessRecord* record = CurrentAccess();
            if (record == nullptr || !record->options.checks) return;
            const std::size_t target = ComponentTypeIDSystem::GetTypeID<T>();
            const bool allowed =
                info_.self_writes.contains(target) ||
                (!write && info_.self_reads.contains(target));
            if (!allowed) {
                constexpr std::string_view kTargetName = TargetName<T>();
                ReportViolation(Violation{
                    .kind = write ? ViolationKind::UndeclaredSelfWrite
                                  : ViolationKind::UndeclaredSelfRead,
                    .type_id = record->type_id,
                    .target_id = target,
                    .service = false,
                    .type_name = record->name,
                    .target_name = kTargetName,
                });
            }
        }
    }

    template<typename T>
    void RequireAny() {
        if constexpr (!kAccessChecksCompiled) {
        } else {
            const AccessRecord* record = CurrentAccess();
            if (record == nullptr || !record->options.checks) return;
            const std::size_t target = ComponentTypeIDSystem::GetTypeID<T>();
            if (!info_.any.contains(target)) {
                constexpr std::string_view kTargetName = TargetName<T>();
                ReportViolation(Violation{
                    .kind = ViolationKind::UndeclaredCrossEntity,
                    .type_id = record->type_id,
                    .target_id = target,
                    .service = false,
                    .type_name = record->name,
                    .target_name = kTargetName,
                });
            }
        }
    }

    ComponentRegistry& registry_;
    const ComponentAccessInfo& info_;
    const ComponentContext* context_;
};

// ── ConfigureGuard ──
//
// The same treatment as UpdateScope for the configure hook: the registry-access
// guard is active (a configure hook may not touch the registry structurally or
// access components), but service validation is off, because configuration is
// serial and no level is in flight.
class ConfigureGuard {
public:
    ConfigureGuard(const ConfigureGuard&) = delete;
    ConfigureGuard& operator=(const ConfigureGuard&) = delete;
    ConfigureGuard(ConfigureGuard&&) = delete;
    ConfigureGuard& operator=(ConfigureGuard&&) = delete;

private:
    friend class ComponentRegistry;

    ConfigureGuard(ComponentRegistry& registry, Component& component,
                   const ComponentAccessInfo& info, const ComponentContext& /*context*/)
        : record_data_{
              .owner = component.GetOwnerId(),
              .type_id = info.type_id,
              .info = &info,
              .name = info.name,
              .options = registry.access_options_,
              .sink = nullptr,
              .mode = AccessMode::Configure,
          },
          record_(record_data_) {}

    AccessRecord record_data_;
    ScopedAccessRecord record_;
};

// ── Out-of-line definitions that need UpdateScope/ConfigureGuard complete ──

inline void ComponentRegistry::ConfigurePass(const ComponentContext& context) {
    if (all_configured_) return;
    for (std::size_t discovery = 0; discovery < infos_.size(); ++discovery) {
        const ComponentAccessInfo& info = infos_[discovery];
        for (std::size_t row = 0; row < type_count_[discovery]; ++row) {
            Component* component = flat_[type_first_[discovery] + row];
            if (component->IsConfigured()) continue;
            ConfigureGuard guard{*this, *component, info, context};
            // Marked only after a successful call, so an exception leaves it
            // unconfigured and the next pass retries.
            component->OnConfigure(context);
            component->MarkConfigured();
        }
    }
    all_configured_ = true;
}

inline void ComponentRegistry::UpdateComponents(float delta_time,
                                                const ComponentContext& context,
                                                VulkanShared::ISystemProfileSink* profile) {
    UpdateActiveGuard guard{update_active_};
    {
        const std::scoped_lock lock(mutex_);
        ThrowIfUpdating("ComponentRegistry::UpdateComponents");
        const ComponentSchedule& schedule = EnsureClosedLocked();
        // The plan is computed before the guard is raised, so the counts cannot
        // change once the pass owns the registry.
        CollectLocked(schedule);
        schedule.ExecutionPlan(unit_counts_, plan_);
        guard.Activate();
    }
    ConfigurePass(context);
    RunPass(delta_time, context, profile);
}

inline void ComponentRegistry::RunUnit(const Unit& unit, float delta_time,
                                       const ComponentContext& context) {
    const ComponentAccessInfo& info = infos_[unit.discovery];
    // One record per work unit: the type, the declaration, the sink and the
    // policy are constant across the unit, so only the owner changes per
    // instance (see UpdateScope).
    const AccessRecord record{
        .owner = EntityId{},
        .type_id = info.type_id,
        .info = &info,
        .name = info.name,
        .options = access_options_,
        .sink = this,
        .mode = AccessMode::Update,
    };
    const ScopedAccessRecord installed{record};
    for (std::size_t row = 0; row < unit.count; ++row) {
        Component* component = flat_[unit.first + row];
        UpdateScope scope{*this, *component, info, context};
        component->Update(delta_time, scope);
    }
}

inline void ComponentRegistry::RunPass(float delta_time, const ComponentContext& context,
                                       VulkanShared::ISystemProfileSink* profile) {
    for (const PlanSegment& segment : plan_) {
        const auto start = std::chrono::steady_clock::now();
        if (segment.parallel) {
            const auto [begin, end] = level_units_[segment.first_level];
            // Resolved here and not at the top of the pass: an all-serial plan
            // must never start the process-wide pool it cannot dispatch to.
            Scheduler().ParallelFor(end - begin, [this, begin, delta_time, &context](
                                                      std::size_t index) {
                RunUnit(units_[begin + index], delta_time, context);
            });
        } else {
            // A sequential segment runs its levels IN ORDER: the merge is a
            // dispatch optimization and never relaxes the barrier.
            for (std::size_t level = segment.first_level;
                 level < segment.first_level + segment.level_count; ++level) {
                const auto [begin, end] = level_units_[level];
                for (std::size_t index = begin; index < end; ++index) {
                    RunUnit(units_[index], delta_time, context);
                }
            }
        }
        if (profile != nullptr) {
            const double milliseconds =
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - start)
                    .count();
            profile->AddSystemTiming(VulkanShared::SystemProfileKind::Component,
                                     SegmentLabel(segment), milliseconds);
        }
    }
}

// ── Out-of-line definitions that need the registry complete ──

inline EntityId Component::GetOwnerId() const noexcept {
    return owner_ != nullptr ? owner_->GetId() : EntityId{};
}

template<typename T>
T* Entity::GetComponent() {
    return registry_ != nullptr ? registry_->GetComponent<T>(*this) : nullptr;
}

template<typename T>
const T* Entity::GetComponent() const {
    return registry_ != nullptr ? registry_->GetComponent<T>(*this) : nullptr;
}

} // namespace VulkanEngine
