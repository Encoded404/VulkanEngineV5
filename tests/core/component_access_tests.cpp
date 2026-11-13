// Component access declarations: the compile-time surface and the spec it
// resolves to.
//
// The declarations here are plain structs deriving AccessDefaults rather than
// Component subclasses, so this file exercises the declaration module alone and
// does not depend on the registry. The negative rows are `static_assert`s on
// `kValidAccessDeclaration`: that predicate exists precisely so a rejected
// declaration is a value a test can assert instead of a compile failure the test
// suite cannot express.

#include <gtest/gtest.h>

import std;

import VulkanEngine.ECS.ComponentAccess;
import VulkanEngine.ECS.Entity;

namespace TestAccess {

using namespace VulkanEngine;

// A plain data type: no declaration members at all.
struct Data {
    int value = 0;
};

// An object component type expressed by the declaration members alone.
struct Plain : AccessDefaults {};

struct NamedOnly : AccessDefaults {
    static constexpr std::string_view kName = "test.named";
};

struct Target : AccessDefaults {
    static constexpr std::string_view kName = "test.target";
};

struct OtherTarget : AccessDefaults {
    static constexpr std::string_view kName = "test.other_target";
};

struct Reader : AccessDefaults {
    using SelfReads = AccessSet<Target>;
    static constexpr std::string_view kName = "test.reader";
};

struct Writer : AccessDefaults {
    using SelfWrites = AccessSet<Target, OtherTarget>;
    static constexpr Concurrency kConcurrency = Concurrency::Serial;
    static constexpr std::uint32_t kServices = ServiceRead(Service::Input);
    static constexpr std::string_view kName = "test.writer";
};

struct Ordered : AccessDefaults {
    using Before = AccessSet<Target>;
    using After = AccessSet<OtherTarget>;
};

// ── Rejected declarations ──
struct SelfParallelWithAny : AccessDefaults {
    using Any = AccessSet<Target>;
    static constexpr Concurrency kConcurrency = Concurrency::SelfParallel;
};

struct SelfParallelWithServiceWrite : AccessDefaults {
    static constexpr Concurrency kConcurrency = Concurrency::SelfParallel;
    static constexpr std::uint32_t kServices = ServiceWrite(Service::Input);
};

struct SelfParallelWithServiceRead : AccessDefaults {
    static constexpr Concurrency kConcurrency = Concurrency::SelfParallel;
    static constexpr std::uint32_t kServices = ServiceRead(Service::Input);
};

struct SelfOrdering : AccessDefaults {
    using Before = AccessSet<SelfOrdering>;
};

struct SelfOrderingAfter : AccessDefaults {
    using After = AccessSet<SelfOrderingAfter>;
};

struct DataOrdering : AccessDefaults {
    using Before = AccessSet<Data>;
};

struct BadConcurrencyType : AccessDefaults {
    using Before = AccessSet<int>;  // not a component type at all
};

// A specialization that tries to smuggle in a rejected combination: it must be
// caught by the same predicate, so moving the sets out of the class is not a
// bypass.
struct SpecializedSelfParallel : AccessDefaults {};

// A type with its own members *and* a specialization: precedence says the
// specialization wins.
struct Overridden : AccessDefaults {
    using SelfReads = AccessSet<Target>;
    static constexpr Concurrency kConcurrency = Concurrency::Serial;
};

} // namespace TestAccess

namespace VulkanEngine {

template<>
struct ComponentAccessTraits<TestAccess::Overridden> : AccessDefaults {
    static constexpr bool kSpecialized = true;
    using SelfWrites = AccessSet<TestAccess::OtherTarget>;
    static constexpr std::string_view kName = "test.overridden";
};

template<>
struct ComponentAccessTraits<TestAccess::SpecializedSelfParallel> : AccessDefaults {
    static constexpr bool kSpecialized = true;
    using Any = AccessSet<TestAccess::Target>;
    static constexpr Concurrency kConcurrency = Concurrency::SelfParallel;
};

} // namespace VulkanEngine

// ── The positive row that proves the predicate is not vacuous ──
static_assert(VulkanEngine::kValidAccessDeclaration<TestAccess::Plain>);
static_assert(VulkanEngine::kValidAccessDeclaration<TestAccess::NamedOnly>);
static_assert(VulkanEngine::kValidAccessDeclaration<TestAccess::Reader>);
static_assert(VulkanEngine::kValidAccessDeclaration<TestAccess::Writer>);
static_assert(VulkanEngine::kValidAccessDeclaration<TestAccess::Ordered>);
static_assert(VulkanEngine::kValidAccessDeclaration<TestAccess::SelfParallelWithServiceRead>);

// ── Every rejected declaration is a value ──
static_assert(!VulkanEngine::kValidAccessDeclaration<TestAccess::SelfParallelWithAny>);
static_assert(!VulkanEngine::kValidAccessDeclaration<TestAccess::SelfParallelWithServiceWrite>);
static_assert(!VulkanEngine::kValidAccessDeclaration<TestAccess::SelfOrdering>);
static_assert(!VulkanEngine::kValidAccessDeclaration<TestAccess::SelfOrderingAfter>);
static_assert(!VulkanEngine::kValidAccessDeclaration<TestAccess::DataOrdering>);
static_assert(!VulkanEngine::kValidAccessDeclaration<TestAccess::BadConcurrencyType>);
static_assert(!VulkanEngine::kValidAccessDeclaration<TestAccess::SpecializedSelfParallel>,
              "a specialization cannot smuggle in SelfParallel with a non-empty Any");
static_assert(!VulkanEngine::kValidAccessDeclaration<TestAccess::Data>);
static_assert(!VulkanEngine::kValidAccessDeclaration<int>);

// The predicate must not reject a legal type through the trait path either.
static_assert(VulkanEngine::kValidAccessDeclaration<TestAccess::Overridden>);

namespace {

using namespace VulkanEngine;
using namespace TestAccess;

TEST(ComponentAccessTest, DefaultsAreExclusiveAndSerialWithNoAccess) {
    constexpr ComponentAccessSpec spec = kAccessSpec<Plain>;
    EXPECT_TRUE(spec.self_reads.empty());
    EXPECT_TRUE(spec.self_writes.empty());
    EXPECT_TRUE(spec.any.empty());
    EXPECT_TRUE(spec.before.empty());
    EXPECT_TRUE(spec.after.empty());
    EXPECT_EQ(spec.services, 0u);
    EXPECT_EQ(spec.concurrency, Concurrency::Exclusive);
    EXPECT_EQ(spec.source, AccessSource::Default);
}

TEST(ComponentAccessTest, MembersProduceTheDeclaredSets) {
    constexpr ComponentAccessSpec spec = kAccessSpec<Writer>;
    ASSERT_EQ(spec.self_writes.size(), 2u);
    EXPECT_TRUE(spec.self_reads.empty());
    EXPECT_EQ(spec.concurrency, Concurrency::Serial);
    EXPECT_EQ(spec.services, ServiceRead(Service::Input));
    EXPECT_EQ(spec.source, AccessSource::Members);
    EXPECT_EQ(spec.name, "test.writer");
}

TEST(ComponentAccessTest, TypeIdResolversYieldTheRuntimeIds) {
    constexpr ComponentAccessSpec spec = kAccessSpec<Reader>;
    ASSERT_EQ(spec.self_reads.size(), 1u);
    ASSERT_NE(spec.self_reads[0].id, nullptr);
    EXPECT_EQ(spec.self_reads[0].id(), ComponentTypeIDSystem::GetTypeID<Target>());
    EXPECT_EQ(spec.self_reads[0].name, "test.target");
}

TEST(ComponentAccessTest, OrderingSetsResolveToTheirIds) {
    constexpr ComponentAccessSpec spec = kAccessSpec<Ordered>;
    ASSERT_EQ(spec.before.size(), 1u);
    ASSERT_EQ(spec.after.size(), 1u);
    EXPECT_EQ(spec.before[0].id(), ComponentTypeIDSystem::GetTypeID<Target>());
    EXPECT_EQ(spec.after[0].id(), ComponentTypeIDSystem::GetTypeID<OtherTarget>());
}

TEST(ComponentAccessTest, MakeAccessSpecNeverAddsTheImplicitSelfWrite) {
    // The registry owns that insertion, so a type's own id must not appear here
    // even though every object component implicitly writes itself.
    constexpr ComponentAccessSpec spec = kAccessSpec<Writer>;
    const std::size_t own = ComponentTypeIDSystem::GetTypeID<Writer>();
    for (const TargetRef& ref : spec.self_writes) {
        EXPECT_NE(ref.id(), own);
    }
}

TEST(ComponentAccessTest, SourceIsContentBasedAndIgnoresTheName) {
    // consteval functions must be invoked in a constant expression.
    constexpr AccessSource plain = AccessSourceOf<Plain>();
    constexpr AccessSource named_only = AccessSourceOf<NamedOnly>();
    constexpr AccessSource reader = AccessSourceOf<Reader>();
    constexpr AccessSource ordered = AccessSourceOf<Ordered>();
    EXPECT_EQ(plain, AccessSource::Default);
    // A name is not an access declaration.
    EXPECT_EQ(named_only, AccessSource::Default);
    EXPECT_EQ(reader, AccessSource::Members);
    EXPECT_EQ(ordered, AccessSource::Members);
}

TEST(ComponentAccessTest, SpecializationWinsOverMembers) {
    constexpr ComponentAccessSpec spec = kAccessSpec<Overridden>;
    EXPECT_EQ(spec.source, AccessSource::Specialization);
    EXPECT_TRUE(spec.self_reads.empty()) << "the class's own SelfReads must be overridden";
    ASSERT_EQ(spec.self_writes.size(), 1u);
    EXPECT_EQ(spec.self_writes[0].id(), ComponentTypeIDSystem::GetTypeID<OtherTarget>());
    EXPECT_EQ(spec.concurrency, Concurrency::Exclusive);
    EXPECT_EQ(spec.name, "test.overridden");
}

TEST(ComponentAccessTest, NameFallsBackToTheCompilerSpelling) {
    constexpr std::string_view named = TargetName<NamedOnly>();
    constexpr std::string_view plain = TargetName<Plain>();
    constexpr std::string_view plain_type = TypeName<Plain>();
    constexpr std::string_view int_name = TypeName<int>();
    constexpr std::string_view double_name = TypeName<double>();

    EXPECT_EQ(named, "test.named");
    EXPECT_FALSE(plain.empty());
    EXPECT_NE(plain, "test.named");
    // TypeName is the fallback, so the two are the same string.
    EXPECT_EQ(plain, plain_type);
    EXPECT_NE(int_name, double_name);
}

TEST(ComponentAccessTest, ServiceBitsAreDisjointPerServiceAndDirection) {
    const std::uint32_t input_read = ServiceRead(Service::Input);
    const std::uint32_t input_write = ServiceWrite(Service::Input);
    const std::uint32_t physics_read = ServiceRead(Service::PhysicsWorld);
    const std::uint32_t physics_write = ServiceWrite(Service::PhysicsWorld);

    EXPECT_NE(input_read, input_write);
    EXPECT_EQ(input_read & input_write, 0u);
    EXPECT_EQ(input_read & physics_read, 0u);
    EXPECT_EQ(input_read & physics_write, 0u);
    EXPECT_EQ(input_write & physics_write, 0u);
    EXPECT_EQ(input_read | input_write, 0b11u);
    EXPECT_EQ(physics_read | physics_write, 0b1100u);
    EXPECT_EQ(ServiceWriteMask(), 0b1010u);
}

TEST(ComponentAccessTest, IsAccessSetRecognizesOnlyTheAlias) {
    EXPECT_TRUE((IsAccessSet<AccessSet<>>));
    EXPECT_TRUE((IsAccessSet<AccessSet<Target, OtherTarget>>));
    EXPECT_FALSE(IsAccessSet<std::tuple<Target>>);
    EXPECT_FALSE(IsAccessSet<int>);
}

} // namespace
