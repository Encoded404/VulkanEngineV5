// Component access declarations: what an object component may touch.
//
// A component type declares its access as nested members (the five AccessSet
// aliases, `kConcurrency`, `kServices`, `kName`) or through a
// `ComponentAccessTraits<T>` specialization. `Component` supplies the
// conservative defaults, so a type that declares nothing is exclusive and
// instance-serial. There is no runtime declaration API: a declaration is a
// property of the type's source, resolved here at compile time and frozen by
// `ComponentSchedule` at close.
//
// This module holds the *declaration* side only. It imports Entity for the
// per-type id source and never the registry, so the schedule can be built and
// tested before any registry change. The registry imports it, not the other way
// round.
//
// The TLS access record also lives here: it is the current component's identity
// for validation and diagnostics, and it deliberately carries no registry
// reference and no `Component*`, so this module stays registry-agnostic while
// still giving the service accessors a reporting path.

module;

export module VulkanEngine.ECS.ComponentAccess;

import std;

import VulkanEngine.ECS.Entity;

export namespace VulkanEngine {

// ── Declarations ──

// One declared set of targets. The element list is carried so a compile-time
// predicate can inspect it without special-casing each alias.
template<typename... Ts>
struct AccessSet {
    using Types = std::tuple<Ts...>;
};

template<typename T>
inline constexpr bool IsAccessSet = false;
template<typename... Ts>
inline constexpr bool IsAccessSet<AccessSet<Ts...>> = true;

// How instances of one type relate to each other and to other types.
enum class Concurrency : std::uint8_t {
    // May not overlap any other type; own instances run serially. The default,
    // which is what makes the engine safe with no annotations.
    Exclusive,
    // Overlaps other types on declared access; own instances run serially.
    Serial,
    // Overlaps other types and its own instances. Only valid with an empty
    // `Any` and no writable service bit.
    SelfParallel,
};

[[nodiscard]] constexpr std::string_view ConcurrencyName(Concurrency concurrency) noexcept {
    switch (concurrency) {
        case Concurrency::Exclusive:    return "exclusive";
        case Concurrency::Serial:       return "serial";
        case Concurrency::SelfParallel: return "self-parallel";
    }
    return "<unknown>";
}

// Engine services a component may use. One read bit and one write bit per
// service are packed into a std::uint32_t.
enum class Service : std::uint8_t {
    Input,
    PhysicsWorld,
    Count,
};
static_assert(std::to_underlying(Service::Count) <= 16,
              "the service mask packs two bits per service into a std::uint32_t");

[[nodiscard]] constexpr std::uint32_t ServiceRead(Service service) noexcept {
    return std::uint32_t{1} << (std::to_underlying(service) * 2);
}
[[nodiscard]] constexpr std::uint32_t ServiceWrite(Service service) noexcept {
    return std::uint32_t{1} << (std::to_underlying(service) * 2 + 1);
}
[[nodiscard]] constexpr std::uint32_t ServiceWriteMask() noexcept {
    std::uint32_t mask = 0;
    for (std::uint8_t i = 0; i < std::to_underlying(Service::Count); ++i) {
        mask |= std::uint32_t{1} << (i * 2 + 1);
    }
    return mask;
}

[[nodiscard]] constexpr std::string_view ServiceName(Service service) noexcept {
    switch (service) {
        case Service::Input:        return "Input";
        case Service::PhysicsWorld: return "PhysicsWorld";
        case Service::Count:        break;
    }
    return "<unknown>";
}

// Where a declaration came from, for the diagnostic dump.
enum class AccessSource : std::uint8_t {
    Default,         // nothing declared; exclusive and serial
    Members,         // the type's own nested members
    Specialization,  // a ComponentAccessTraits<T> specialization
};

[[nodiscard]] constexpr std::string_view AccessSourceName(AccessSource source) noexcept {
    switch (source) {
        case AccessSource::Default:        return "default";
        case AccessSource::Members:        return "members";
        case AccessSource::Specialization: return "specialization";
    }
    return "<unknown>";
}

// What a detected undeclared access does.
enum class ViolationAction : std::uint8_t {
    LogOnce,  // log the first occurrence per (type, target, kind), drop the rest
    Throw,    // throw std::logic_error on the violation
};

// ── Compile-time names ──

// A readable spelling of a type, extracted from the compiler's signature string.
// It is implementation-formatted, not mangled: stable across build
// configurations of one compiler (which is what cross-configuration dump
// comparison needs), but never stable across compilers and never the same thing
// as an author-supplied `kName`.
template<typename T>
[[nodiscard]] consteval std::string_view TypeName() {
#if defined(_MSC_VER) && !defined(__clang__)
    constexpr std::string_view signature = __FUNCSIG__;
    constexpr std::string_view marker = "TypeName<";
    constexpr auto open = signature.find(marker);
    constexpr auto close = signature.rfind('>');
    static_assert(open != std::string_view::npos && close != std::string_view::npos,
                  "TypeName could not parse __FUNCSIG__");
    constexpr auto begin = open + marker.size();
    static_assert(close > begin, "TypeName parsed an empty name from __FUNCSIG__");
    return signature.substr(begin, close - begin);
#else
    constexpr std::string_view signature = __PRETTY_FUNCTION__;
    constexpr std::string_view marker = "[T = ";
    constexpr auto open = signature.find(marker);
    constexpr auto close = signature.rfind(']');
    static_assert(open != std::string_view::npos && close != std::string_view::npos,
                  "TypeName could not parse __PRETTY_FUNCTION__");
    constexpr auto begin = open + marker.size();
    static_assert(close > begin, "TypeName parsed an empty name from __PRETTY_FUNCTION__");
    return signature.substr(begin, close - begin);
#endif
}

#if defined(__clang__) || defined(__GNUC__)
static_assert(TypeName<int>() == "int", "TypeName extraction is broken on this compiler");
#elif defined(_MSC_VER)
static_assert(TypeName<int>().ends_with("int"), "TypeName extraction is broken on this compiler");
#endif

// The name to print for a target or a declaring type: the author's `kName` when
// the type declares a non-empty one, otherwise the compiler spelling.
template<typename T>
[[nodiscard]] consteval std::string_view TargetName() {
    if constexpr (requires { T::kName; }) {
        if constexpr (std::same_as<std::remove_cvref_t<decltype(T::kName)>,
                                   std::string_view>) {
            if (!T::kName.empty()) return T::kName;
        }
    }
    return TypeName<T>();
}

// ── The defaults a component inherits ──

// These are exactly the members `Component` supplies. A component derives them
// by inheriting `Component`; a `ComponentAccessTraits` specialization derives
// them by inheriting this, because a full specialization inherits nothing from
// its primary template.
struct AccessDefaults {
    static constexpr bool kSpecialized = false;
    using SelfReads = AccessSet<>;
    using SelfWrites = AccessSet<>;
    using Any = AccessSet<>;
    using Before = AccessSet<>;
    using After = AccessSet<>;
    static constexpr Concurrency kConcurrency = Concurrency::Exclusive;
    static constexpr std::uint32_t kServices = 0;
    static constexpr std::string_view kName{};
};

template<typename T>
struct ComponentAccessTraits : AccessDefaults {};

// ── Compile-time validation ──

// True when `T` itself has the declaration surface: the five AccessSet aliases,
// `kConcurrency` and `kServices`. This is what makes a data component a
// compile error rather than a close-time "not registered" log; `kName` is
// deliberately not part of it, because every data component carries one.
template<typename T>
concept DeclaresAccess = requires {
    typename T::SelfReads;
    typename T::SelfWrites;
    typename T::Any;
    typename T::Before;
    typename T::After;
    requires IsAccessSet<typename T::SelfReads>;
    requires IsAccessSet<typename T::SelfWrites>;
    requires IsAccessSet<typename T::Any>;
    requires IsAccessSet<typename T::Before>;
    requires IsAccessSet<typename T::After>;
    // A compound requirement tests decltype((E)), which is `const Concurrency&`
    // for a static constexpr member; comparing against `Concurrency` directly
    // would reject every legal type.
    requires std::same_as<std::remove_cvref_t<decltype(T::kConcurrency)>, Concurrency>;
    { T::kServices } -> std::convertible_to<std::uint32_t>;
};

// Precedence: a specialization wins over the type's own members.
template<typename T>
using DeclarationSource =
    std::conditional_t<ComponentAccessTraits<T>::kSpecialized, ComponentAccessTraits<T>, T>;

namespace access_detail {

template<typename Self, typename... Ts>
[[nodiscard]] consteval bool ValidOrderingTargets(std::tuple<Ts...>*) {
    // An ordering target must be an object component type (only those have
    // nodes) and must not be the declaring type itself (intra-type overlap is
    // the concurrency mode's job, and a self-edge would make every close fail
    // with a misleading cycle).
    return ((DeclaresAccess<Ts> && !std::same_as<Ts, Self>) && ...);
}

template<typename Self, typename List>
[[nodiscard]] consteval bool ValidOrderingList() {
    if constexpr (!IsAccessSet<List>) {
        return false;
    } else {
        return ValidOrderingTargets<Self>(static_cast<typename List::Types*>(nullptr));
    }
}

template<typename List>
[[nodiscard]] consteval bool IsEmptyAccessSet() {
    if constexpr (!IsAccessSet<List>) {
        return false;
    } else {
        return std::tuple_size_v<typename List::Types> == 0;
    }
}

} // namespace access_detail

// The pure decision behind MakeAccessSpec's static_assert. Every branch is
// guarded by `if constexpr`, so a data type evaluates to false instead of
// hard-erroring: that is what makes the negative tests instantiable.
template<typename T>
[[nodiscard]] consteval bool ValidAccessDeclaration() {
    using Decl = DeclarationSource<T>;
    if constexpr (!DeclaresAccess<Decl>) {
        return false;
    } else {
        constexpr bool ordering_ok =
            access_detail::ValidOrderingList<T, typename Decl::Before>() &&
            access_detail::ValidOrderingList<T, typename Decl::After>();
        constexpr bool self_parallel_ok = [] {
            if (Decl::kConcurrency != Concurrency::SelfParallel) return true;
            // Cross-entity access is read-write (unschedulable for its own
            // instances) and a service write would race them.
            return access_detail::IsEmptyAccessSet<typename Decl::Any>() &&
                   (Decl::kServices & ServiceWriteMask()) == 0;
        }();
        return ordering_ok && self_parallel_ok;
    }
}

template<typename T>
inline constexpr bool kValidAccessDeclaration = ValidAccessDeclaration<T>();

// The content rule behind AccessSource, shared by the spec and the dump. `kName`
// is deliberately not part of it: a type that only sets a name declares no
// access and stays Default.
template<typename T>
[[nodiscard]] consteval AccessSource AccessSourceOf() {
    if constexpr (ComponentAccessTraits<T>::kSpecialized) {
        return AccessSource::Specialization;
    } else if constexpr (!DeclaresAccess<T>) {
        return AccessSource::Default;
    } else {
        constexpr bool declares_nothing =
            access_detail::IsEmptyAccessSet<typename T::SelfReads>() &&
            access_detail::IsEmptyAccessSet<typename T::SelfWrites>() &&
            access_detail::IsEmptyAccessSet<typename T::Any>() &&
            access_detail::IsEmptyAccessSet<typename T::Before>() &&
            access_detail::IsEmptyAccessSet<typename T::After>() && T::kServices == 0 &&
            T::kConcurrency == Concurrency::Exclusive;
        return declares_nothing ? AccessSource::Default : AccessSource::Members;
    }
}

// ── The compile-time spec ──

// A resolver yields a target's numeric type id at close. Type ids are a lazily
// initialized static (Entity.cppm), so they are not constant expressions; the
// function pointer is, which is why the spec can hold one and never needs the id
// at compile time.
using TypeIdFn = std::size_t (*)();

struct TargetRef {
    TypeIdFn id = nullptr;
    std::string_view name{};
};

struct ComponentAccessSpec {
    std::span<const TargetRef> self_reads{};
    std::span<const TargetRef> self_writes{};
    std::span<const TargetRef> any{};
    std::span<const TargetRef> before{};
    std::span<const TargetRef> after{};
    std::uint32_t services = 0;
    Concurrency concurrency = Concurrency::Exclusive;
    std::string_view name{};
    AccessSource source = AccessSource::Default;
};

template<typename... Ts>
[[nodiscard]] consteval auto ResolversOf(AccessSet<Ts...>) {
    return std::array<TargetRef, sizeof...(Ts)>{
        TargetRef{&ComponentTypeIDSystem::GetTypeID<Ts>, TargetName<Ts>()}...,
    };
}

// The owner of a spec's target arrays. The arrays live here rather than in
// `static constexpr` locals of `MakeAccessSpec`, because a static local of a
// `consteval` function is only ever read during constant evaluation: Clang 21
// accepts it but emits the emitted object zero-filled, so a span into it
// dereferences null function pointers at runtime. A variable template's static
// data members are emitted normally, with relocations, on every toolchain.
template<typename T>
struct SpecStorage {
    using Decl = DeclarationSource<T>;

    static constexpr auto self_reads = ResolversOf(typename Decl::SelfReads{});
    static constexpr auto self_writes = ResolversOf(typename Decl::SelfWrites{});
    static constexpr auto any = ResolversOf(typename Decl::Any{});
    static constexpr auto before = ResolversOf(typename Decl::Before{});
    static constexpr auto after = ResolversOf(typename Decl::After{});
};

template<typename T>
[[nodiscard]] consteval ComponentAccessSpec MakeAccessSpec() {
    static_assert(DeclaresAccess<DeclarationSource<T>>,
                  "only object components declare access: a type needs the five AccessSet "
                  "aliases, kConcurrency and kServices (Component supplies them) or a "
                  "ComponentAccessTraits specialization. A kName does not count.");
    static_assert(kValidAccessDeclaration<T>,
                  "illegal component access declaration; see "
                  "docs/component-access-and-scheduling.md");

    if constexpr (!DeclaresAccess<DeclarationSource<T>>) {
        // Already diagnosed above; the empty spec keeps the error count at one
        // instead of cascading into SpecStorage's member types.
        return {};
    } else {
        using Decl = DeclarationSource<T>;
        return ComponentAccessSpec{
            .self_reads = SpecStorage<T>::self_reads,
            .self_writes = SpecStorage<T>::self_writes,
            .any = SpecStorage<T>::any,
            .before = SpecStorage<T>::before,
            .after = SpecStorage<T>::after,
            .services = Decl::kServices,
            .concurrency = Decl::kConcurrency,
            .name = TargetName<Decl>(),
            .source = AccessSourceOf<T>(),
        };
    }
}

template<typename T>
inline constexpr ComponentAccessSpec kAccessSpec = MakeAccessSpec<T>();

// ── The resolved (numeric) form ──

struct ComponentAccessInfo {
    std::size_t type_id = 0;
    std::size_t discovery_index = 0;
    std::string_view name{};
    AccessSource source = AccessSource::Default;
    Concurrency concurrency = Concurrency::Exclusive;
    std::uint32_t services = 0;
    std::flat_set<std::size_t> self_reads;
    std::flat_set<std::size_t> self_writes;  // own id inserted at close
    std::flat_set<std::size_t> any;
    std::flat_set<std::size_t> before;
    std::flat_set<std::size_t> after;

    friend bool operator==(const ComponentAccessInfo& lhs, const ComponentAccessInfo& rhs) {
        return lhs.type_id == rhs.type_id && lhs.discovery_index == rhs.discovery_index &&
               lhs.name == rhs.name && lhs.source == rhs.source &&
               lhs.concurrency == rhs.concurrency && lhs.services == rhs.services &&
               std::ranges::equal(lhs.self_reads, rhs.self_reads) &&
               std::ranges::equal(lhs.self_writes, rhs.self_writes) &&
               std::ranges::equal(lhs.any, rhs.any) &&
               std::ranges::equal(lhs.before, rhs.before) &&
               std::ranges::equal(lhs.after, rhs.after);
    }
};

struct NameEntry {
    std::size_t type_id = 0;
    std::string_view name{};

    friend bool operator==(const NameEntry&, const NameEntry&) = default;
};
using NameTable = std::span<const NameEntry>;

// ── Options ──

// Whether the validation code exists at all. The code is compiled out under
// NDEBUG unless VKENGINE_COMPONENT_CHECKS is defined; `checks` and
// `on_violation` are runtime switches on top of that.
#if defined(VKENGINE_COMPONENT_CHECKS) || !defined(NDEBUG)
inline constexpr bool kAccessChecksCompiled = true;
#else
inline constexpr bool kAccessChecksCompiled = false;
#endif

#if defined(VKENGINE_COMPONENT_CHECKS)
inline constexpr bool kAccessChecksDefault = true;
inline constexpr ViolationAction kViolationDefault = ViolationAction::Throw;
#elif !defined(NDEBUG)
inline constexpr bool kAccessChecksDefault = true;
inline constexpr ViolationAction kViolationDefault = ViolationAction::LogOnce;
#else
inline constexpr bool kAccessChecksDefault = false;
inline constexpr ViolationAction kViolationDefault = ViolationAction::LogOnce;
#endif

struct ComponentAccessOptions {
    bool checks = kAccessChecksDefault;
    ViolationAction on_violation = kViolationDefault;

    friend bool operator==(const ComponentAccessOptions&, const ComponentAccessOptions&) = default;
};

// ── Violations ──

enum class ViolationKind : std::uint8_t {
    UndeclaredSelfRead,
    UndeclaredSelfWrite,
    UndeclaredCrossEntity,
    UndeclaredServiceRead,
    UndeclaredServiceWrite,
    SelfParallelWithAny,
    SelfParallelWithServiceWrite,
};

[[nodiscard]] constexpr std::string_view ViolationKindName(ViolationKind kind) noexcept {
    switch (kind) {
        case ViolationKind::UndeclaredSelfRead:        return "undeclared self read";
        case ViolationKind::UndeclaredSelfWrite:       return "undeclared self write";
        case ViolationKind::UndeclaredCrossEntity:     return "undeclared cross-entity access";
        case ViolationKind::UndeclaredServiceRead:     return "undeclared service read";
        case ViolationKind::UndeclaredServiceWrite:    return "undeclared service write";
        case ViolationKind::SelfParallelWithAny:       return "SelfParallel with a non-empty Any";
        case ViolationKind::SelfParallelWithServiceWrite:
            return "SelfParallel with a writable service bit";
    }
    return "<unknown>";
}

struct Violation {
    ViolationKind kind = ViolationKind::UndeclaredSelfRead;
    std::size_t type_id = 0;     // the declaring component type
    std::size_t target_id = 0;   // the component type id, or the service index
    bool service = false;        // true when `target_id` is a Service index
    std::string_view type_name{};
    std::string_view target_name{};
};

// A one-method interface rather than std::function: the report path must not
// allocate, and `std::move_only_function` is not implemented in libc++ 21.
class IViolationSink {
public:
    virtual ~IViolationSink() = default;
    virtual void OnViolation(const Violation& violation) = 0;
};

[[nodiscard]] std::string FormatViolation(const Violation& violation);

// ── The TLS access record ──
//
// The current component's identity while its Update/OnConfigure runs. It stores
// no `Component*` and no registry reference, so this module never depends on the
// registry, while the service accessors still get a reporting path.

enum class AccessMode : std::uint8_t {
    None,
    Configure,
    Update,
};

struct AccessRecord {
    EntityId owner{};
    std::size_t type_id = 0;
    const ComponentAccessInfo* info = nullptr;
    std::string_view name{};
    ComponentAccessOptions options{};
    IViolationSink* sink = nullptr;
    AccessMode mode = AccessMode::None;
};

[[nodiscard]] AccessRecord* CurrentAccess() noexcept;
[[nodiscard]] AccessMode CurrentAccessMode() noexcept;

// The executor installs one record per work unit and updates only the owner per
// component: re-installing the whole record for every instance costs more than
// the component's own Update does, which is a lot to pay on a hot path.
void SetCurrentAccessOwner(EntityId owner) noexcept;

// RAII: installs a record for the duration of one component call and restores
// whatever was there before. Neither copyable nor movable, so it cannot outlive
// the call it belongs to.
class ScopedAccessRecord {
public:
    ScopedAccessRecord(const ScopedAccessRecord&) = delete;
    ScopedAccessRecord& operator=(const ScopedAccessRecord&) = delete;
    ScopedAccessRecord(ScopedAccessRecord&&) = delete;
    ScopedAccessRecord& operator=(ScopedAccessRecord&&) = delete;

    explicit ScopedAccessRecord(const AccessRecord& record) noexcept;
    ~ScopedAccessRecord();

private:
    AccessRecord saved_{};
};

// Reports through the active record's sink when it has one and checks are on.
// A violation with no active record (setup, a system, a test) is a no-op: the
// validator guards the barriered path, it is not a capability system.
void ReportViolation(const Violation& violation);

// Called by the service accessors. Only meaningful in `Update` mode: it checks
// the declaring type's service mask and reports a violation otherwise. A write
// bit satisfies a read.
void EnforceServiceAccess(Service service, bool write);

} // namespace VulkanEngine

// ── Out-of-line definitions ──
//
// These were declared inside the export block above, so they are exported; the
// TLS slot itself stays internal to this module, which is why every accessor
// goes through a function instead of naming the variable.
namespace VulkanEngine {

namespace {

thread_local AccessRecord g_access_record{};

} // namespace

AccessRecord* CurrentAccess() noexcept {
    return g_access_record.mode == AccessMode::None ? nullptr : &g_access_record;
}

AccessMode CurrentAccessMode() noexcept {
    return g_access_record.mode;
}

void SetCurrentAccessOwner(EntityId owner) noexcept {
    if (g_access_record.mode == AccessMode::None) return;
    g_access_record.owner = owner;
}

ScopedAccessRecord::ScopedAccessRecord(const AccessRecord& record) noexcept : saved_{g_access_record} {
    g_access_record = record;
}

ScopedAccessRecord::~ScopedAccessRecord() {
    g_access_record = saved_;
}

void ReportViolation(const Violation& violation) {
    if constexpr (!kAccessChecksCompiled) {
        (void)violation;
    } else {
        AccessRecord* record = CurrentAccess();
        if (record == nullptr || record->sink == nullptr || !record->options.checks) return;
        record->sink->OnViolation(violation);
    }
}

void EnforceServiceAccess(Service service, bool write) {
    if constexpr (!kAccessChecksCompiled) {
        (void)service;
        (void)write;
    } else {
        AccessRecord* record = CurrentAccess();
        if (record == nullptr || record->mode != AccessMode::Update || record->info == nullptr) {
            return;
        }
        if (!record->options.checks) return;
        const std::uint32_t required =
            write ? ServiceWrite(service) : (ServiceRead(service) | ServiceWrite(service));
        if ((record->info->services & required) != 0) return;
        ReportViolation(Violation{
            .kind = write ? ViolationKind::UndeclaredServiceWrite
                          : ViolationKind::UndeclaredServiceRead,
            .type_id = record->type_id,
            .target_id = std::to_underlying(service),
            .service = true,
            .type_name = record->name,
            .target_name = ServiceName(service),
        });
    }
}

std::string FormatViolation(const Violation& violation) {
    if (violation.service) {
        return std::format("component '{}' performed a {} on service '{}'",
                           violation.type_name, ViolationKindName(violation.kind),
                           violation.target_name);
    }
    return std::format("component '{}' performed a {} on '{}'", violation.type_name,
                       ViolationKindName(violation.kind), violation.target_name);
}

} // namespace VulkanEngine
