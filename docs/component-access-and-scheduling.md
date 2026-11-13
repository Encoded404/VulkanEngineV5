# Component access and scheduling

This is the normative contract for object-component scheduling. It describes what
a component type may declare, how those declarations become an execution order,
and what the engine guarantees while that order runs. Code lives in
`engine/core/ecs/ComponentAccess.cppm` (declarations, the compile-time spec, the
TLS access record), `engine/core/ecs/ComponentSchedule.cppm` (the resolver) and
`engine/core/ecs/ComponentRegistry.cppm` (registration, the executor, the
validator).

The short version: an object component type declares what it reads and writes,
the declarations resolve once into a graph of levels, and the engine runs those
levels — never two conflicting types at the same time. A type that declares
nothing is exclusive, which is the safe default.

## Targets

Every object component type declares four sets, all over component type ids:

| Set | Meaning |
|---|---|
| `SelfReads<C...>` | reads its **own entity's** component `C` |
| `SelfWrites<C...>` | writes its **own entity's** component `C` |
| `Any<C...>` | reads or writes **any entity's** component `C` (queries, lookups by `EntityId`) |
| `kServices` | bitmask of engine services it uses, read and/or write |

`Any` is deliberately coarse: cross-entity access is read-write, it conflicts
with every access to `C`, and it forces the declaring type's instances to
serialize. Fine-grained cross-entity read tracking would double the graph for
little benefit; bulk work belongs in a data system.

Only **object component types** are schedule nodes. Data components (`Transform`,
`RigidBody`, `Collider`, `Character`, `DynamicMesh`, …) are targets only: they
have no `Update`, so they never appear in the graph. Declaring access on a data
type is a compile error — `MakeAccessSpec<T>` requires the declaration members,
which a plain struct does not have. A `kName` does not count: every data
component carries one.

## Concurrency modes

One enum names the three legal modes, so the fourth (both flags set) cannot be
written:

| Mode | Conflicts with | Its own instances |
|---|---|---|
| `Concurrency::Exclusive` (default) | **every** other registered type | serial |
| `Concurrency::Serial` | types it shares access with | serial |
| `Concurrency::SelfParallel` | types it shares access with | **may overlap** |

`SelfParallel` is valid only with an empty `Any` and no writable service bit: a
cross-entity access is read-write (so its own instances would race) and a service
write would race them too. That restriction is a `static_assert`.

Explicit ordering is `Before<C...>` / `After<C...>`. It is independent of
conflicts: it orders two types that may not touch the same data at all.

## Declaring

A type declares with nested members; `Component` supplies the conservative
defaults, so an existing type compiles unchanged and is exclusive:

```cpp
class SimpleControllerComponent : public Component {
public:
    using SelfReads  = AccessSet<Components::Transform>;
    using SelfWrites = AccessSet<Components::Transform>;
    static constexpr Concurrency kConcurrency = Concurrency::SelfParallel;
    static constexpr std::uint32_t kServices = ServiceRead(Service::Input);
    static constexpr std::string_view kName = "examples.simple_controller";
};
```

A type whose source cannot be edited uses a `ComponentAccessTraits<T>`
specialization, which is exactly equivalent and goes through the same code path:

```cpp
template<>
struct ComponentAccessTraits<ThirdParty::Mover> : AccessDefaults {
    static constexpr bool kSpecialized = true;   // the only member that must be set
    using SelfWrites = AccessSet<Components::Transform>;
    static constexpr Concurrency kConcurrency = Concurrency::Serial;
    static constexpr std::string_view kName = "thirdparty.mover";
};
```

A full specialization inherits nothing from its primary template, which is why it
derives `AccessDefaults` and overrides only what it changes. A specialization
wins over the type's own members.

`AccessSource` records where the declaration came from — `Default` (nothing
declared), `Members`, or `Specialization` — and the dump prints it. The member
path is content-based: because every object component inherits the members from
`Component`, a presence check cannot tell "declared nothing" from "declared the
default". `kName` is not part of that rule; a type that only sets a name declares
no access and stays `Default`.

### Rejected declarations

A rejected declaration is a `static_assert` whose *decision* is also a value
(`kValidAccessDeclaration<T>`), so the test suite can assert it:

| Rejected | Why |
|---|---|
| `SelfParallel` with a non-empty `Any` | cross-entity access is read-write and unschedulable for its own instances |
| `SelfParallel` with a writable service bit | the write would race its own instances |
| a data-typed `Before`/`After` target | only object components have nodes |
| a self-referential `Before`/`After` | intra-type overlap is the concurrency mode's job |
| `MakeAccessSpec<C>` for a type without the declaration members | data types cannot be declared |

A cycle is not a `static_assert`: it depends on which types are registered
together, so it is reported at registration close.

## Two representations

- `ComponentAccessSpec` is built by a `consteval MakeAccessSpec<T>()`. It stores
  **type-id resolvers**, not numeric ids, because a type id comes from a lazily
  initialized static and is not a constant expression. At close each resolver is
  called once and the spec is discarded.
- `ComponentAccessInfo` is the numeric form, built once at close and then
  read-only. Its id sets are `std::flat_set`, so "sorted and unique" is a
  property of the container instead of a hand-maintained invariant.

Every object component type implicitly **writes itself**: its own `Update`
mutates its own members. The registry inserts the type's own id into
`self_writes` at close — `MakeAccessSpec` never does, so there is exactly one
owner of that insertion. Without it a component reading another component on the
same entity would not conflict with that component's own update.

## Registration lifecycle

A type becomes a node at its **first use**:

- `AddComponent<T>`, `OnAdd<T>` and `OnRemove<T>` register `T` if it is an object
  type;
- `RegisterComponent<T>()` registers it explicitly, for a type that is registered
  by engine setup rather than by its first instance;
- read-only use (`GetComponent`, `HasComponent`, `Count`) never registers
  anything: a lookup cannot create a node.

Registration assigns a discovery index (a monotonic counter in registration
order), appends the spec and the node, and does **not** resolve anything.

Registration closes at the first call that needs the schedule — the first
`UpdateComponents`, or an explicit `CloseRegistration()`. The correct moment for
the explicit form is after the application has registered its component types: at
the end of its setup hook, or just before its first update. It must **not** be
called from `EngineBootstrap::Initialize`, which runs before app setup: closing
there resolves a zero-node schedule and every later `AddComponent` throws.

Closing does this, in order:

1. fill each info: call the spec's target resolvers, drop `Before`/`After` targets
   that are not registered here (one warning naming each — the constraint is
   vacuous, and treating it as an error would break libraries whose optional
   ordering names a type the host did not use), and insert the implicit
   self-write;
2. store the name table;
3. `ComponentSchedule::Resolve(infos, names)` builds the graph;
4. a cycle is returned as a `ScheduleError`, stored, and rethrown as the same
   `std::logic_error` text by every later call. Nothing was constructed and no
   schedule was published;
5. on success the schedule is stored. This is the only mutation the graph ever
   sees.

After a successful close the node set, the discovery indices, the levels and the
edges never change. Registering a new type throws `std::logic_error` (the message
names the type and `RegisterComponent<T>()`). A type with zero live instances is
still a node; it contributes zero work and `Describe()` shows it as registered
but empty. `Clear()` drops entities and pools and **keeps** the registrations and
the schedule.

Re-registration is not a concept. A program that needs a different type set
constructs a different registry.

## Conflicts, edges and direction

For distinct types `A` and `B`:

```
Writes(X) = X.self_writes ∪ X.any        (any is read-write)
Reads(X)  = X.self_reads  ∪ X.any
```

They conflict when any of:

1. `A.concurrency == Exclusive || B.concurrency == Exclusive`;
2. `Writes(A) ∩ (Reads(B) ∪ Writes(B))` is non-empty, or the symmetric case;
3. their service masks conflict: a write of service `S` in one overlaps any read
   or write of `S` in the other; read/read does not conflict.

Same-type instances are not compared by these rules: they overlap exactly when
`concurrency == SelfParallel`.

Both explicit ordering and conflicts become edges in the same DAG. A conflicting
pair gets exactly one directed edge, chosen in this order:

1. a direct explicit relation on the pair (the edge already exists);
2. otherwise, the direction along an existing explicit path between the two;
3. otherwise, the fallback that does **not** close a cycle: try the lower
   discovery index first, and if that direction would close a cycle while the
   reverse would not, use the reverse.

**The pair-visit order is part of the contract.** Explicit edges are inserted
first; conflicting pairs are then visited in a fixed double loop over discovery
order (`i < j`), each oriented against the graph built so far. Direction, levels
and the dump are therefore a function of the declarations and the registration
order alone, never of insertion history. Reordering the same declarations is a
*different* schedule: the determinism claim is one registration order → one
answer, not that reordering is level-invariant.

At most one direction can close a cycle in a DAG, so "both directions close a
cycle" is an internal invariant violation and is asserted, not reported. The only
user-facing cycle comes from the explicit subgraph.

## Levels and units

The resolver takes Kahn's algorithm, always picking the lowest discovery index
that is currently free. Levels are cut from that total order: a new level starts
whenever the next node has a predecessor already in the current level. A level is
therefore a maximal run with no intra-level edge, and it is a **barrier**: every
type in level *n* finishes before any type in level *n+1* starts.

An exclusive type has an edge to every other registered type, so a level is
either a single exclusive type or a set of non-exclusive types. Exclusive types
never share a level.

Within a level there are no edges, so execution is one `ParallelFor` over **work
units**:

- a `SelfParallel` type contributes one unit **per instance**;
- any other type contributes **one** many-instance unit (zero units when it has
  no live instances), whose instances run serially inside it.

That keeps the two concurrency questions separate: `concurrency != Exclusive`
means "may overlap *other* types"; `concurrency == SelfParallel` means "instances
of this type may overlap *each other*".

A **level** is parallel iff its own work list holds more than one unit — a
per-step question, because it depends on live instance counts. A lone
non-exclusive type with one instance is serial, not a wasted `ParallelFor`.

A **plan segment** is either one parallel level, or a maximal run of consecutive
serial levels. The merge is a dispatch optimization that saves a pool round trip;
it never reorders: the levels inside a segment are still barriers with respect to
each other, and a segment with more than one level is always sequential.

"Pool row order" is the pool's dense order, which is insertion order until a
removal: `Remove` is a swap-delete, so a removed middle row is refilled by the
former last row. It is deterministic for a given structural history, but it is
not insertion order and no test may pin it across a removal.

## Why no per-entity islands

`Self<C>` accesses from different entities are disjoint by construction: one
component instance per type per entity, so two instances of the same type can
never share an owner. Cross-type pairs that touch the same `C` are separated by a
conflict edge and never run in the same level. Therefore:

- same-type instances that touch only `Self` are safe in parallel iff the type is
  `SelfParallel`;
- cross-type instances that touch the same target are never in the same level;
- `Any` breaks the per-entity argument, which is why it forbids `SelfParallel`
  and conflicts with everything touching `C`.

Entity-level partitioning would only recover parallelism between *different*
types touching the same target on *different* entities.

## The update pass

`ComponentRegistry::UpdateComponents(delta_time, context, profile)`:

1. take the mutex, close registration if needed, collect every object component
   into one flat buffer with per-type ranges and per-type unit counts;
2. compute the plan into a reused buffer;
3. raise `update_active_` through an RAII guard, still holding the mutex, then
   release it;
4. run `OnConfigure` for every instance that has not been configured yet,
   serially, in registration order then pool order;
5. walk the plan: a parallel segment runs one `ParallelFor` over its level's
   units; a sequential segment runs its levels in order, each level's units in
   that level's own order.

The plan is computed before the flag is raised, which is what makes it
consistent: the counts were collected under the same lock, and nothing can change
them once the pass owns the registry.

The scheduler is injected as a nullable member: `ComponentRegistry(JobScheduler* =
nullptr)` falls back to `JobScheduler::Global()` on first use. Tests pass a local
pool to prove overlap; production leaves it null. A reference default would start
the process-wide pool at `EngineContext` construction, before `EngineBootstrap`
sizes it.

## UpdateScope: the only registry path

```cpp
class UpdateScope {
public:
    UpdateScope(const UpdateScope&) = delete;  // …and no move, either
    [[nodiscard]] EntityId SelfId() const noexcept;
    template<typename T> T* Find();                  // own entity, needs a write
    template<typename T> const T* Read();            // own entity, read or write
    template<typename T> T* Find(EntityId);
    template<typename T> const T* Read(EntityId);
    template<typename... Ts, typename Fn> void ForEach(Fn&&);
    [[nodiscard]] const ComponentContext& Services() const noexcept;
    void Destroy(EntityId);
    template<typename T> void Remove(EntityId);
private:
    friend class ComponentRegistry;
    UpdateScope(...);   // the executor is the only construction site
};
```

The private constructor and the deleted special members are the capability: a
public constructor would let any code holding a registry forge a scope, and a
movable scope could outlive the call and leave the TLS slot open.

Rules:

- `Find<T>()` requires `T ∈ self_writes`; `Read<T>()` accepts `self_reads ∪
  self_writes`.
- `Find<T>(id)` / `Read<T>(id)` require `T ∈ Any`. The id may equal self; the
  declaration is then the superset and the access is recorded as cross-entity.
- `ForEach<Ts...>` requires every `Ts ∈ Any` and accepts both callable shapes the
  registry's `ForEach` accepts: `fn(EntityId, Ts&...)` and `fn(Ts&...)`. The
  callback receives the visited `EntityId`, so it can queue a destroy or do a
  further scoped lookup without a raw `Entity*`.
- `Destroy` / `Remove` queue on the registry's pending list; nothing is freed
  while a level is running.
- `Services()` returns the granted `ComponentContext`, whose accessors validate
  the service bits.

`Component::GetOwner()` returns `const Entity*` (identity only). The entity is
not a registry handle during an update: use the scope.

## Lifecycle hooks

- `OnAttach(Entity&)` / `OnDetach(Entity&)` keep their semantics: they run
  serialized under the registry mutex and are **not** part of the barrier graph.
- `OnConfigure(const ComponentContext&)` runs once per instance, at the start of
  the first update pass that sees it, after the instance collection and before
  the first level. It is serial by construction, so a service write there cannot
  race a level; that is where a one-time input binding belongs. It may not touch
  the registry structurally or access components. A `ConfigureGuard` sets the TLS
  mode to `Configure`: the registry-access guard is active but service validation
  is off. The configured flag lives on the instance, not the type slot, and is
  set only after a successful call, so an exception leaves the component
  unconfigured and the next pass retries.
- `Update(float, UpdateScope&)` is the only place a component runs. `OnAttach` is
  not a place to bind services: it runs inside `AddComponent` under the registry
  mutex, before the engine knows which services are granted.

## Service grants

`ComponentContext`'s grant fields are private. The engine and tests set them with
`GrantInput` / `GrantPhysicsWorld`; components read them with `Input()` (write
grant), `InputRead()` (const read), `PhysicsWorld()` and `PhysicsWorldRead()`.
A write bit satisfies a read, so a declared writer may call `InputRead()` while a
declared reader calling `Input()` is rejected. Readers do not serialize against
other readers.

The accessors validate only while the TLS mode is `Update`. Outside an update
(setup, systems, tests) they return the granted pointer unvalidated: the check
guards the barriered path, it is not a capability system for arbitrary calls.

## Structural rules during a pass

While `update_active_` is true, **every** public registry member rejects on
**every** thread: `CreateEntity`, `AddComponent`, `DestroyEntity`,
`RemoveComponent`, `Clear`, `ApplyStructuralChanges`, `UpdateComponents`,
`ForEach`, `ForEachEntity`, `GetComponent`, `HasComponent`, `Count`,
`EntityCount`, `TryGetEntity`, `IsAlive`, `OnAdd`, `OnRemove`,
`RegisterComponent`, `CloseRegistration`, `SetAccessOptions`, `Schedule`,
`AccessInfo`, `TargetNames`, `ExecutionPlan` and `Describe`.

The mutators would mutate pools a worker is reading; the read-only members are
closed too because `TryGetEntity` and `ForEachEntity` hand out `Entity&`, whose
`GetComponent` yields mutable pointers. The public schedule views throw while the
executor works from the plan it computed before the flag was raised. The public
`Clear()` throws; the destructor calls a private `ClearNoThrow()`, because
`~ComponentRegistry()` is implicitly `noexcept`.

The flag is not itself the synchronization. `UpdateComponents` raises it while
holding the mutex, and every member that takes the mutex re-checks it **after**
acquiring it — the call before the lock is only an early-out, because a thread
that passed it can block on the mutex and acquire it after a pass has started.

The lock-free reads (`GetComponent`, `HasComponent`, `IsAlive`, `TryGetEntity`)
test the flag once and do not take the mutex: the whole point of that path is to
avoid the lock, so its check is an early-out rather than a fence. A query that
*starts* during a pass is rejected; a query already in flight when the pass
begins can overlap its first moments. Those reads only touch a pool's sparse
index, which no pass mutates, so the overlap is a stale read at worst — a
structural change is what would invalidate it, and those all queue or throw. `DestroyEntity` / `RemoveComponent` additionally
throw when called from inside a component (the TLS record says so) — a component
must use `scope.Destroy` / `scope.Remove` — and otherwise queue when
`update_active_ || iteration_depth_ > 0`.

`AddComponent` and `CreateEntity` throw when `update_active_ || iteration_depth_
> 0`. The `iteration_depth_` half is a **pre-lock** check on purpose: `ForEach`
holds the non-recursive mutex across its callback, so a post-lock test from
inside a callback would block instead of throwing.

`ApplyStructuralChanges` takes the mutex, checks the flag inside it, swaps the
queue under `pending_mutex_`, and applies the actions **while still holding the
mutex**. A pre-swap check outside the mutex would leave the window where the check
passes and an update then starts.

Lock order is always `mutex_ → pending_mutex_`.

## Validation

The validator is best effort and the limits are part of the contract.

```cpp
enum class ViolationAction : std::uint8_t { LogOnce, Throw };

struct ComponentAccessOptions {
    bool checks = /* true unless NDEBUG and no VKENGINE_COMPONENT_CHECKS */;
    ViolationAction on_violation = /* Throw with VKENGINE_COMPONENT_CHECKS, else LogOnce */;
};
```

The code is compiled out under `NDEBUG` unless the `VKENGINE_COMPONENT_CHECKS`
CMake option (default `OFF`) is on, which adds the definition to the engine target
PUBLIC so the engine and every test target agree. That option only decides whether
the validation code exists; `checks` and `on_violation` are runtime switches on
top. With the option on, `on_violation` defaults to `Throw`, so a Release build
cannot be green while checking nothing.

`UpdateScope` validates every access against the declaring type's resolved info:
an undeclared self fetch, a cross-entity fetch or `ForEach` without `Any`, a
service accessor without the matching bit. The de-duplication set is
registry-owned (a mutex plus an unordered set), not thread-local — a thread-local
copy would report once per worker, which is not "once". The lock is only taken
when a violation is about to be reported.

Two bypasses are documented and accepted: a component can cache the service
pointer it obtained during `OnConfigure`, and `Action<D>` itself carries an
`InputSystem*`, so a call through a stored action never reaches the accessor.
Both still require the declaration. Closing them would mean wrapping every
service method in a phase-checked facade.

The validator cannot see a cached `T*` fetched before the update, raw memory
reached through a service, statics inside the component, or writes through a
pointer obtained outside the registry. It is a guard for the barriered path, not
a sandbox.

## Diagnostics

`ComponentSchedule::Describe()` prints the static structure — levels, edges,
declarations — with `std::format_to`. `Level`, `PlanSegment`, `EdgeRecord` and
`ComponentAccessInfo` carry defaulted `operator==` so tests compare snapshots
instead of parsing text.

```
component schedule: 3 types, 3 levels
level 0 (1 type):
  simple_controller  [members]  mode:self-parallel  self r:Transform w:Transform,simple_controller  services r:Input
level 1 (1 type):
  transform_control  [members]  mode:self-parallel  self r:- w:Transform,transform_control
level 2 (1 type):
  debug_draw         [members]  mode:exclusive      self w:debug_draw  any:Transform
edges:
  simple_controller -> transform_control  (conflict: write/write Transform, discovery order)
  simple_controller -> debug_draw         (exclusive, discovery order)
  transform_control  -> debug_draw        (exclusive, discovery order)
```

Every edge is printed, including the ones implied by exclusivity: the direction is
not a property of "exclusive", only the existence of the edge is, and a dump that
showed only the "interesting" edges would disagree with the levels the resolver
produced. Every row prints the resolved `self_writes`, which always contains the
type's own id, so the dump shows exactly what the validator accepts.

**Names cover targets too.** Every component type resolves a name: its `kName`
when it declares a non-empty one, otherwise a `consteval TypeName<T>()` extracted
from the compiler's signature string. `TypeName` is implementation-formatted, not
mangled, and is stable across build configurations of one compiler — which is what
the cross-configuration dump comparison needs. It is not stable across compilers
and is never the same thing as an author-supplied `kName`.

The structure is static: it is produced once at close and does not depend on the
entity population. `ExecutionPlan()` (between steps) is what reflects live counts
and merging.

Profiling reports one timing per **executed segment** under
`SystemProfileKind::Component`. The label is `component.parallel[<level>]` for a
parallel segment, `component.serial[<first>-<last>]` for a multi-level sequential
segment, and `component.serial[<level>]` for a single-level one. The index is a
**level** index, so a dump can be cross-referenced against `Levels()`. Per-instance
timings are deliberately not added: the segment is the unit that can be attributed
without atomics in the hot path.

## Determinism

- Within a build, registration order, pair-visit order, edge direction, levels and
  within-level order are deterministic for the same declarations and the same
  registration sequence.
- Type ids are process-lifetime and assigned on first use. They are stable within
  a run and relative to each other, but not across code changes. Names are the
  author-facing identity; ids are the runtime key.
- The schedule is frozen at close; there is no runtime insertion, reordering or
  re-registration.
- `Describe()` is comparable across build configurations of one compiler, not
  across compilers.

## Not in this contract

- **No runtime declaration, revision or re-registration API.** A declaration is
  fixed by the type's source. A *grow-only* revision (add reads/writes/services,
  keep flags and ordering) would be sound; a general one is not, because removing
  an access removes a conflict edge while the `Update` body still performs the
  access. If a hot-reload path ever needs it, it is `Resolve`-and-swap with
  exactly that rule.
- **No compile-time typed scopes.** A CRTP bridge would turn undeclared access
  into a compile error and delete the validator, `checks`, `on_violation` and the
  de-duplication set. It is not taken because it changes `Component` to
  `ComponentBase<Self>` for every type and makes the checks absolute: the runtime
  validator can be compiled out for shipping, a compile-time check cannot. The
  structural guard stays either way, because it is about pool mutation.
- **No deferred spawning from `Update`.** Spawning stays a system /
  phase-boundary operation.
- **No per-entity scheduling islands.** Type-level is the contract until measured
  otherwise.

## Strict configuration

```
cmake --preset check          && cmake --build --preset check-debug   && ctest --preset check-debug
cmake --build --preset check-release   && ctest --preset check-release
cmake --preset default        && cmake --build --preset release      && ctest --preset release
```

`check-release` verifies strict validation under `NDEBUG`; the last line is the
shipping configuration, where the validator is compiled out. The repository has no
CI configuration, so this is the documented local gate.
