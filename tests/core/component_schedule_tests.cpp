// The component schedule: conflicts, edge direction, levels and the per-step
// execution plan.
//
// The fixtures build `ComponentAccessInfo` values directly, so the resolver is
// tested without a registry. That is deliberate: `Resolve` is the pure function
// the registry calls once, and pinning its behaviour here is what keeps the
// registry a thin wrapper.

#include <gtest/gtest.h>

import std;

import VulkanEngine.ECS.ComponentAccess;
import VulkanEngine.ECS.ComponentSchedule;
import VulkanEngine.ECS.ComponentContext;
import VulkanEngine.ECS.ComponentRegistry;

namespace {

using namespace VulkanEngine;

// A resolved info with the implicit self-write the registry inserts at close.
[[nodiscard]] ComponentAccessInfo MakeInfo(std::size_t id, std::string_view name,
                                           Concurrency concurrency = Concurrency::Exclusive,
                                           std::uint32_t services = 0) {
    ComponentAccessInfo info;
    info.type_id = id;
    info.discovery_index = id;
    info.name = name;
    info.concurrency = concurrency;
    info.services = services;
    info.source = AccessSource::Members;
    info.self_writes.insert(id);
    return info;
}

// The schedule stores a span over the infos it was resolved from, so the fixture
// owns both and hands out the schedule through them. This is the same invariant
// the registry keeps: no resize after close.
struct ResolvedSchedule {
    std::vector<ComponentAccessInfo> infos;
    ComponentSchedule schedule;

    [[nodiscard]] std::span<const Level> Levels() const noexcept { return schedule.Levels(); }
    [[nodiscard]] std::span<const EdgeRecord> Edges() const noexcept { return schedule.Edges(); }
    [[nodiscard]] std::string Describe() const { return schedule.Describe(); }
    [[nodiscard]] const ComponentAccessInfo* Info(std::size_t type_id) const {
        return schedule.Info(type_id);
    }
    [[nodiscard]] std::string_view NameOf(std::size_t type_id) const {
        return schedule.NameOf(type_id);
    }
    void ExecutionPlan(std::span<const std::size_t> units, std::vector<PlanSegment>& out) const {
        schedule.ExecutionPlan(units, out);
    }
    [[nodiscard]] std::vector<PlanSegment> ExecutionPlan(
        std::span<const std::size_t> units) const {
        return schedule.ExecutionPlan(units);
    }
};

[[nodiscard]] std::shared_ptr<const ResolvedSchedule>
ResolveOrFail(std::vector<ComponentAccessInfo> infos, NameTable names = {}) {
    auto holder = std::make_shared<ResolvedSchedule>();
    holder->infos = std::move(infos);
    // The registry appends infos in registration order, so Resolve expects the
    // span sorted by discovery index. The fixtures build them in declaration
    // order instead, so normalize here.
    std::ranges::stable_sort(holder->infos, {}, &ComponentAccessInfo::discovery_index);
    auto result = ComponentSchedule::Resolve(holder->infos, names);
    if (!result) {
        ADD_FAILURE() << result.error().message;
        return nullptr;
    }
    holder->schedule = std::move(*result);
    return holder;
}

[[nodiscard]] std::vector<std::size_t> LevelSizes(const ResolvedSchedule& schedule) {
    std::vector<std::size_t> sizes;
    for (const Level& level : schedule.Levels()) sizes.push_back(level.type_ids.size());
    return sizes;
}

// Type ids here are indices, so the name of a level is its type ids in order.
[[nodiscard]] std::vector<std::vector<std::size_t>> LevelContents(
    const ResolvedSchedule& schedule) {
    std::vector<std::vector<std::size_t>> out;
    for (const Level& level : schedule.Levels()) out.push_back(level.type_ids);
    return out;
}

} // namespace

TEST(ComponentScheduleTest, EmptyRegistrationResolvesToNothing) {
    std::vector<ComponentAccessInfo> infos;
    auto result = ComponentSchedule::Resolve(infos, {});
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->Levels().empty());
    EXPECT_TRUE(result->Edges().empty());
    EXPECT_EQ(result->Describe(), "component schedule: 0 types, 0 levels\nedges:\n");
}

TEST(ComponentScheduleTest, OneTypeIsOneLevelWithNoEdges) {
    const auto schedule = ResolveOrFail({MakeInfo(0, "alpha")});
    ASSERT_NE(schedule, nullptr);
    EXPECT_EQ(LevelContents(*schedule), (std::vector<std::vector<std::size_t>>{{0}}));
    EXPECT_TRUE(schedule->Edges().empty());
}

TEST(ComponentScheduleTest, ExclusiveTypesNeverShareALevel) {
    const auto schedule = ResolveOrFail({MakeInfo(0, "alpha"), MakeInfo(1, "bravo"),
                                         MakeInfo(2, "charlie")});
    ASSERT_NE(schedule, nullptr);
    EXPECT_EQ(LevelSizes(*schedule), (std::vector<std::size_t>{1, 1, 1}));
    ASSERT_EQ(schedule->Edges().size(), 3u);
    for (const EdgeRecord& edge : schedule->Edges()) {
        EXPECT_TRUE(std::holds_alternative<ExclusiveEdge>(edge.cause));
    }
    // Discovery order orients each pair forward, so the chain runs in
    // registration order.
    EXPECT_EQ(schedule->Edges()[0].before, 0u);
    EXPECT_EQ(schedule->Edges()[0].after, 1u);
    EXPECT_EQ(schedule->Edges()[1].before, 0u);
    EXPECT_EQ(schedule->Edges()[1].after, 2u);
    EXPECT_EQ(schedule->Edges()[2].before, 1u);
    EXPECT_EQ(schedule->Edges()[2].after, 2u);
}

TEST(ComponentScheduleTest, ReadReadDoesNotConflict) {
    auto alpha = MakeInfo(0, "alpha", Concurrency::Serial);
    alpha.self_reads.insert(10);
    auto bravo = MakeInfo(1, "bravo", Concurrency::Serial);
    bravo.self_reads.insert(10);

    const auto schedule = ResolveOrFail({alpha, bravo});
    ASSERT_NE(schedule, nullptr);
    EXPECT_TRUE(schedule->Edges().empty());
    EXPECT_EQ(LevelContents(*schedule), (std::vector<std::vector<std::size_t>>{{0, 1}}));
}

TEST(ComponentScheduleTest, ReadWriteConflictsAndReportsTheTarget) {
    auto alpha = MakeInfo(0, "alpha", Concurrency::Serial);
    alpha.self_writes.insert(10);
    auto bravo = MakeInfo(1, "bravo", Concurrency::Serial);
    bravo.self_reads.insert(10);

    NameEntry names[] = {{10, "target"}};
    const auto schedule = ResolveOrFail({alpha, bravo}, names);
    ASSERT_NE(schedule, nullptr);
    ASSERT_EQ(schedule->Edges().size(), 1u);
    const EdgeRecord& edge = schedule->Edges()[0];
    EXPECT_EQ(edge.before, 0u);
    EXPECT_EQ(edge.after, 1u);
    EXPECT_EQ(edge.direction, DirectionSource::DiscoveryOrder);
    const auto* cause = std::get_if<ConflictEdge>(&edge.cause);
    ASSERT_NE(cause, nullptr);
    EXPECT_EQ(cause->target, 10u);
    EXPECT_FALSE(cause->write_write);
    EXPECT_EQ(schedule->NameOf(10), "target");
}

TEST(ComponentScheduleTest, WriteWriteConflicts) {
    auto alpha = MakeInfo(0, "alpha", Concurrency::Serial);
    alpha.self_writes.insert(10);
    auto bravo = MakeInfo(1, "bravo", Concurrency::Serial);
    bravo.self_writes.insert(10);

    const auto schedule = ResolveOrFail({alpha, bravo});
    ASSERT_NE(schedule, nullptr);
    ASSERT_EQ(schedule->Edges().size(), 1u);
    const auto* cause = std::get_if<ConflictEdge>(&schedule->Edges()[0].cause);
    ASSERT_NE(cause, nullptr);
    EXPECT_TRUE(cause->write_write);
}

TEST(ComponentScheduleTest, AnyConflictsWithSelfAccess) {
    auto alpha = MakeInfo(0, "alpha", Concurrency::Serial);
    alpha.any.insert(10);
    auto bravo = MakeInfo(1, "bravo", Concurrency::Serial);
    bravo.self_reads.insert(10);

    const auto schedule = ResolveOrFail({alpha, bravo});
    ASSERT_NE(schedule, nullptr);
    ASSERT_EQ(schedule->Edges().size(), 1u);
    const auto* cause = std::get_if<ConflictEdge>(&schedule->Edges()[0].cause);
    ASSERT_NE(cause, nullptr);
    EXPECT_EQ(cause->target, 10u);
}

TEST(ComponentScheduleTest, AnyConflictsWithAny) {
    auto alpha = MakeInfo(0, "alpha", Concurrency::Serial);
    alpha.any.insert(10);
    auto bravo = MakeInfo(1, "bravo", Concurrency::Serial);
    bravo.any.insert(10);

    const auto schedule = ResolveOrFail({alpha, bravo});
    ASSERT_NE(schedule, nullptr);
    ASSERT_EQ(schedule->Edges().size(), 1u);
    EXPECT_TRUE(std::holds_alternative<ConflictEdge>(schedule->Edges()[0].cause));
}

TEST(ComponentScheduleTest, ServiceReadsDoNotConflictButWritesDo) {
    auto alpha = MakeInfo(0, "alpha", Concurrency::Serial, ServiceRead(Service::Input));
    auto bravo = MakeInfo(1, "bravo", Concurrency::Serial, ServiceRead(Service::Input));

    const auto readers = ResolveOrFail({alpha, bravo});
    ASSERT_NE(readers, nullptr);
    EXPECT_TRUE(readers->Edges().empty());
    EXPECT_EQ(readers->Levels().size(), 1u);

    auto charlie = MakeInfo(2, "charlie", Concurrency::Serial, ServiceWrite(Service::Input));
    const auto mixed = ResolveOrFail({alpha, bravo, charlie});
    ASSERT_NE(mixed, nullptr);
    ASSERT_EQ(mixed->Edges().size(), 2u);
    for (const EdgeRecord& edge : mixed->Edges()) {
        const auto* cause = std::get_if<ServiceConflictEdge>(&edge.cause);
        ASSERT_NE(cause, nullptr);
        EXPECT_EQ(cause->service, Service::Input);
        EXPECT_FALSE(cause->write_write);
        EXPECT_EQ(edge.after, 2u);
    }
}

TEST(ComponentScheduleTest, ServiceWriteWriteIsReportedAsSuch) {
    auto alpha = MakeInfo(0, "alpha", Concurrency::Serial, ServiceWrite(Service::PhysicsWorld));
    auto bravo = MakeInfo(1, "bravo", Concurrency::Serial, ServiceWrite(Service::PhysicsWorld));

    const auto schedule = ResolveOrFail({alpha, bravo});
    ASSERT_NE(schedule, nullptr);
    ASSERT_EQ(schedule->Edges().size(), 1u);
    const auto* cause = std::get_if<ServiceConflictEdge>(&schedule->Edges()[0].cause);
    ASSERT_NE(cause, nullptr);
    EXPECT_EQ(cause->service, Service::PhysicsWorld);
    EXPECT_TRUE(cause->write_write);
}

TEST(ComponentScheduleTest, ExplicitOrderingReversesDiscoveryOrder) {
    auto alpha = MakeInfo(0, "alpha");
    auto bravo = MakeInfo(1, "bravo", Concurrency::Serial);
    bravo.before.insert(0);

    const auto schedule = ResolveOrFail({alpha, bravo});
    ASSERT_NE(schedule, nullptr);
    ASSERT_EQ(schedule->Edges().size(), 1u);
    EXPECT_EQ(schedule->Edges()[0].before, 1u);
    EXPECT_EQ(schedule->Edges()[0].after, 0u);
    EXPECT_EQ(schedule->Edges()[0].direction, DirectionSource::Explicit);
    EXPECT_TRUE(std::holds_alternative<ExplicitEdge>(schedule->Edges()[0].cause));
    EXPECT_EQ(LevelContents(*schedule), (std::vector<std::vector<std::size_t>>{{1}, {0}}));
}

TEST(ComponentScheduleTest, ExplicitOrderingSeparatesNonConflictingTypes) {
    auto alpha = MakeInfo(0, "alpha", Concurrency::SelfParallel);
    auto bravo = MakeInfo(1, "bravo", Concurrency::SelfParallel);
    alpha.before.insert(1);

    const auto schedule = ResolveOrFail({alpha, bravo});
    ASSERT_NE(schedule, nullptr);
    ASSERT_EQ(schedule->Edges().size(), 1u);
    EXPECT_EQ(schedule->Edges()[0].direction, DirectionSource::Explicit);
    EXPECT_EQ(LevelContents(*schedule), (std::vector<std::vector<std::size_t>>{{0}, {1}}));
}

TEST(ComponentScheduleTest, ExplicitChainResolvesRegardlessOfRegistrationOrder) {
    auto build = [](std::size_t alpha, std::size_t bravo, std::size_t charlie) {
        auto a = MakeInfo(alpha, "alpha", Concurrency::SelfParallel);
        a.self_writes.insert(10);
        a.before.insert(bravo);
        auto b = MakeInfo(bravo, "bravo", Concurrency::SelfParallel);
        b.self_writes.insert(10);
        b.before.insert(charlie);
        auto c = MakeInfo(charlie, "charlie", Concurrency::SelfParallel);
        c.self_writes.insert(10);
        return std::vector<ComponentAccessInfo>{a, b, c};
    };

    // Registration order alpha, bravo, charlie.
    {
        const auto schedule = ResolveOrFail(build(0, 1, 2));
        ASSERT_NE(schedule, nullptr);
        EXPECT_EQ(LevelContents(*schedule),
                  (std::vector<std::vector<std::size_t>>{{0}, {1}, {2}}));
        EXPECT_EQ(schedule->Edges().size(), 3u);
    }
    // Registration order charlie, bravo, alpha: the same declarations resolve.
    {
        const auto schedule = ResolveOrFail(build(2, 1, 0));
        ASSERT_NE(schedule, nullptr);
        EXPECT_EQ(LevelContents(*schedule),
                  (std::vector<std::vector<std::size_t>>{{2}, {1}, {0}}));
        EXPECT_EQ(schedule->Edges().size(), 3u);
    }
}

TEST(ComponentScheduleTest, FixedPairOrderFlipsAConflictThatWouldCloseARing) {
    // bravo(0), charlie(1), alpha(2), only `alpha Before<bravo>`.
    // Pair (bravo,charlie) gives bravo -> charlie; pair (charlie,alpha) would
    // give charlie -> alpha by discovery order, but that closes
    // alpha -> bravo -> charlie -> alpha, so it flips to alpha -> charlie.
    auto bravo = MakeInfo(0, "bravo", Concurrency::SelfParallel);
    bravo.self_writes.insert(10);
    auto charlie = MakeInfo(1, "charlie", Concurrency::SelfParallel);
    charlie.self_writes.insert(10);
    auto alpha = MakeInfo(2, "alpha", Concurrency::SelfParallel);
    alpha.self_writes.insert(10);
    alpha.before.insert(0);

    const auto schedule = ResolveOrFail({bravo, charlie, alpha});
    ASSERT_NE(schedule, nullptr);
    EXPECT_EQ(LevelContents(*schedule), (std::vector<std::vector<std::size_t>>{{2}, {0}, {1}}));

    std::vector<std::pair<std::size_t, std::size_t>> edges;
    for (const EdgeRecord& edge : schedule->Edges()) {
        edges.emplace_back(edge.before, edge.after);
    }
    const std::vector<std::pair<std::size_t, std::size_t>> expected{
        {2, 0}, {0, 1}, {2, 1}};
    EXPECT_EQ(edges, expected);
}

TEST(ComponentScheduleTest, ReorderingTheDeclarationsIsADifferentSchedule) {
    // charlie(0), bravo(1), alpha(2) with only `alpha Before<bravo>`.
    auto charlie = MakeInfo(0, "charlie", Concurrency::SelfParallel);
    charlie.self_writes.insert(10);
    auto bravo = MakeInfo(1, "bravo", Concurrency::SelfParallel);
    bravo.self_writes.insert(10);
    auto alpha = MakeInfo(2, "alpha", Concurrency::SelfParallel);
    alpha.self_writes.insert(10);
    alpha.before.insert(1);

    const auto schedule = ResolveOrFail({charlie, bravo, alpha});
    ASSERT_NE(schedule, nullptr);
    EXPECT_EQ(LevelContents(*schedule), (std::vector<std::vector<std::size_t>>{{0}, {2}, {1}}));

    std::vector<std::pair<std::size_t, std::size_t>> edges;
    for (const EdgeRecord& edge : schedule->Edges()) {
        edges.emplace_back(edge.before, edge.after);
    }
    // The explicit edge is inserted first, then the conflicts in pair order.
    const std::vector<std::pair<std::size_t, std::size_t>> expected{
        {2, 1}, {0, 1}, {0, 2}};
    EXPECT_EQ(edges, expected);
}

TEST(ComponentScheduleTest, ExplicitCycleIsReportedWithTypeIds) {
    auto alpha = MakeInfo(0, "alpha");
    alpha.before.insert(1);
    auto bravo = MakeInfo(1, "bravo");
    bravo.before.insert(2);
    auto charlie = MakeInfo(2, "charlie");
    charlie.before.insert(0);

    const std::vector<ComponentAccessInfo> infos{alpha, bravo, charlie};
    auto result = ComponentSchedule::Resolve(infos, {});
    ASSERT_FALSE(result.has_value());
    const ScheduleError& error = result.error();
    EXPECT_EQ(error.cycle, (std::vector<std::size_t>{0, 1, 2}));
    EXPECT_NE(error.message.find("alpha"), std::string::npos) << error.message;
    EXPECT_NE(error.message.find("bravo"), std::string::npos) << error.message;
    EXPECT_NE(error.message.find("charlie"), std::string::npos) << error.message;
    EXPECT_NE(error.message.find(" -> "), std::string::npos) << error.message;
}

TEST(ComponentScheduleTest, ResolutionIsDeterministic) {
    auto alpha = MakeInfo(0, "alpha", Concurrency::SelfParallel);
    alpha.self_writes.insert(10);
    auto bravo = MakeInfo(1, "bravo", Concurrency::SelfParallel);
    bravo.self_reads.insert(10);
    auto charlie = MakeInfo(2, "charlie");

    const auto first = ResolveOrFail({alpha, bravo, charlie});
    const auto second = ResolveOrFail({alpha, bravo, charlie});
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(first->Levels().size(), second->Levels().size());
    for (std::size_t i = 0; i < first->Levels().size(); ++i) {
        EXPECT_EQ(first->Levels()[i], second->Levels()[i]);
    }
    ASSERT_EQ(first->Edges().size(), second->Edges().size());
    for (std::size_t i = 0; i < first->Edges().size(); ++i) {
        EXPECT_EQ(first->Edges()[i], second->Edges()[i]);
    }
    EXPECT_EQ(first->Describe(), second->Describe());
}

TEST(ComponentScheduleTest, ExecutionPlanMergesSerialLevelsIntoOneSegment) {
    const auto schedule =
        ResolveOrFail({MakeInfo(0, "alpha"), MakeInfo(1, "bravo"), MakeInfo(2, "charlie")});
    ASSERT_NE(schedule, nullptr);
    ASSERT_EQ(schedule->Levels().size(), 3u);

    const std::array<std::size_t, 3> units{1, 1, 1};
    const std::vector<PlanSegment> plan = schedule->ExecutionPlan(units);
    ASSERT_EQ(plan.size(), 1u);
    EXPECT_EQ(plan[0], (PlanSegment{0, 3, false}));
}

TEST(ComponentScheduleTest, ExecutionPlanSplitsAParallelLevelOut) {
    auto alpha = MakeInfo(0, "alpha", Concurrency::Serial);
    alpha.self_writes.insert(10);
    auto bravo = MakeInfo(1, "bravo", Concurrency::SelfParallel);
    bravo.self_reads.insert(10);
    auto charlie = MakeInfo(2, "charlie", Concurrency::SelfParallel);
    charlie.self_reads.insert(10);
    auto delta = MakeInfo(3, "delta");

    const auto schedule = ResolveOrFail({alpha, bravo, charlie, delta});
    ASSERT_NE(schedule, nullptr);
    ASSERT_EQ(schedule->Levels().size(), 3u);
    EXPECT_EQ(LevelContents(*schedule),
              (std::vector<std::vector<std::size_t>>{{0}, {1, 2}, {3}}));

    const std::array<std::size_t, 4> units{1, 2, 2, 1};
    const std::vector<PlanSegment> plan = schedule->ExecutionPlan(units);
    ASSERT_EQ(plan.size(), 3u);
    EXPECT_EQ(plan[0], (PlanSegment{0, 1, false}));
    EXPECT_EQ(plan[1], (PlanSegment{1, 1, true}));
    EXPECT_EQ(plan[2], (PlanSegment{2, 1, false}));
}

TEST(ComponentScheduleTest, ExecutionPlanMergesConsecutiveEmptyLevels) {
    // Two registered types with no live instances: zero units each, so both
    // levels are serial and merge.
    const auto schedule = ResolveOrFail({MakeInfo(0, "alpha"), MakeInfo(1, "bravo")});
    ASSERT_NE(schedule, nullptr);
    const std::array<std::size_t, 2> units{0, 0};
    const std::vector<PlanSegment> plan = schedule->ExecutionPlan(units);
    ASSERT_EQ(plan.size(), 1u);
    EXPECT_FALSE(plan[0].parallel);
    EXPECT_EQ(plan[0].level_count, 2u);
}

TEST(ComponentScheduleTest, DescribePrintsLevelsNamesAndEdgeCauses) {
    auto alpha = MakeInfo(0, "alpha", Concurrency::SelfParallel);
    alpha.self_writes.insert(10);
    alpha.self_reads.insert(10);
    auto bravo = MakeInfo(1, "bravo", Concurrency::SelfParallel);
    bravo.self_writes.insert(10);
    auto delta = MakeInfo(2, "delta");
    delta.any.insert(10);

    NameEntry names[] = {{10, "Transform"}};
    const auto schedule = ResolveOrFail({alpha, bravo, delta}, names);
    ASSERT_NE(schedule, nullptr);

    const std::string dump = schedule->Describe();
    EXPECT_NE(dump.find("component schedule: 3 types, 3 levels"), std::string::npos) << dump;
    EXPECT_NE(dump.find("level 0 (1 type):"), std::string::npos) << dump;
    EXPECT_NE(dump.find("[members]"), std::string::npos) << dump;
    EXPECT_NE(dump.find("mode:self-parallel"), std::string::npos) << dump;
    EXPECT_NE(dump.find("self r:Transform w:alpha,Transform"), std::string::npos) << dump;
    EXPECT_NE(dump.find("any:Transform"), std::string::npos) << dump;
    EXPECT_NE(dump.find("alpha -> bravo  (conflict: write/write Transform, discovery order)"),
              std::string::npos)
        << dump;
    EXPECT_NE(dump.find("(exclusive, discovery order)"), std::string::npos) << dump;
    // No id fallback anywhere: every row and every edge resolved a name.
    EXPECT_EQ(dump.find("component_type["), std::string::npos) << dump;
}

TEST(ComponentScheduleTest, InfoAndNameOfReachTheResolvedRecords) {
    auto alpha = MakeInfo(0, "alpha");
    const auto schedule = ResolveOrFail({alpha});
    ASSERT_NE(schedule, nullptr);
    const ComponentAccessInfo* info = schedule->Info(0);
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->name, "alpha");
    EXPECT_TRUE(info->self_writes.contains(0));
    EXPECT_EQ(schedule->Info(99), nullptr);
    EXPECT_EQ(schedule->NameOf(99), "");
}

TEST(ComponentScheduleTest, DirectionSourceNamesAreStable) {
    EXPECT_EQ(DirectionSourceName(DirectionSource::Explicit), "explicit");
    EXPECT_EQ(DirectionSourceName(DirectionSource::ExplicitPath), "explicit path");
    EXPECT_EQ(DirectionSourceName(DirectionSource::DiscoveryOrder), "discovery order");
    EXPECT_EQ(ConcurrencyName(Concurrency::SelfParallel), "self-parallel");
    EXPECT_EQ(AccessSourceName(AccessSource::Specialization), "specialization");
}

// ── The registry-facing matrix ──
//
// The same schedule, but driven by real component declarations and a real
// registry, so registration order, the implicit self-write and the frozen
// schedule are exercised end to end.

namespace {

using namespace VulkanEngine;

// A data-typed target: never a schedule node, always a conflict target.
struct RegistryTarget : Component {
    static constexpr std::string_view kName = "test.registry_target";
};

struct WritesRegistryTarget : Component {
    using SelfWrites = AccessSet<RegistryTarget>;
    static constexpr Concurrency kConcurrency = Concurrency::Serial;
    static constexpr std::string_view kName = "test.writes_registry_target";
};

struct ReadsRegistryTarget : Component {
    using SelfReads = AccessSet<RegistryTarget>;
    static constexpr Concurrency kConcurrency = Concurrency::Serial;
    static constexpr std::string_view kName = "test.reads_registry_target";
};

struct AbsentOrderingTarget : Component {
    static constexpr std::string_view kName = "test.absent_ordering_target";
};

struct OrdersTowardsAbsent : Component {
    using Before = AccessSet<AbsentOrderingTarget>;
    static constexpr std::string_view kName = "test.orders_towards_absent";
};

} // namespace

TEST(ComponentScheduleRegistryTest, DeclarationsResolveIntoLevelsThroughTheRegistry) {
    ComponentRegistry registry;
    Entity& entity = registry.CreateEntity();
    registry.AddComponent<WritesRegistryTarget>(entity);
    registry.AddComponent<ReadsRegistryTarget>(entity);

    const ComponentSchedule& schedule = registry.CloseRegistration();
    ASSERT_EQ(schedule.Levels().size(), 2u);
    EXPECT_EQ(schedule.Levels()[0].type_ids,
              (std::vector<std::size_t>{ComponentTypeIDSystem::GetTypeID<WritesRegistryTarget>()}));
    EXPECT_EQ(schedule.Levels()[1].type_ids,
              (std::vector<std::size_t>{ComponentTypeIDSystem::GetTypeID<ReadsRegistryTarget>()}));
    ASSERT_EQ(schedule.Edges().size(), 1u);
    const auto* cause = std::get_if<ConflictEdge>(&schedule.Edges()[0].cause);
    ASSERT_NE(cause, nullptr);
    EXPECT_EQ(cause->target, ComponentTypeIDSystem::GetTypeID<RegistryTarget>());
    EXPECT_FALSE(cause->write_write);
}

TEST(ComponentScheduleRegistryTest, TheRegistryInsertsTheImplicitSelfWrite) {
    ComponentRegistry registry;
    Entity& entity = registry.CreateEntity();
    registry.AddComponent<WritesRegistryTarget>(entity);
    (void)registry.CloseRegistration();

    const ComponentAccessInfo* info =
        registry.AccessInfo(ComponentTypeIDSystem::GetTypeID<WritesRegistryTarget>());
    ASSERT_NE(info, nullptr);
    EXPECT_TRUE(info->self_writes.contains(ComponentTypeIDSystem::GetTypeID<WritesRegistryTarget>()))
        << "a type implicitly writes itself; the registry owns that insertion";
    EXPECT_TRUE(info->self_writes.contains(ComponentTypeIDSystem::GetTypeID<RegistryTarget>()));
    EXPECT_EQ(info->name, "test.writes_registry_target");
    EXPECT_EQ(info->source, AccessSource::Members);
    EXPECT_EQ(info->concurrency, Concurrency::Serial);

    // A type that is not a node has no info.
    EXPECT_EQ(registry.AccessInfo(ComponentTypeIDSystem::GetTypeID<RegistryTarget>()), nullptr);
}

TEST(ComponentScheduleRegistryTest, AnUnregisteredOrderingTargetIsDroppedWithoutBlockingTheClose) {
    ComponentRegistry registry;
    Entity& entity = registry.CreateEntity();
    registry.AddComponent<OrdersTowardsAbsent>(entity);

    // `AbsentOrderingTarget` is registered in no registry: the constraint is
    // vacuous, so the close succeeds and produces one node with no edges.
    const ComponentSchedule& schedule = registry.CloseRegistration();
    ASSERT_EQ(schedule.Levels().size(), 1u);
    EXPECT_TRUE(schedule.Edges().empty());
    const ComponentAccessInfo* info =
        registry.AccessInfo(ComponentTypeIDSystem::GetTypeID<OrdersTowardsAbsent>());
    ASSERT_NE(info, nullptr);
    EXPECT_TRUE(info->before.empty());

    // The target name still resolves for the dump, even though it is not a node.
    bool found = false;
    for (const NameEntry& entry : registry.TargetNames()) {
        if (entry.type_id == ComponentTypeIDSystem::GetTypeID<AbsentOrderingTarget>()) {
            found = true;
            EXPECT_EQ(entry.name, "test.absent_ordering_target");
        }
    }
    EXPECT_TRUE(found);
}

TEST(ComponentScheduleRegistryTest, ClearKeepsRegistrationsAndTheResolvedSchedule) {
    ComponentRegistry registry;
    Entity& entity = registry.CreateEntity();
    registry.AddComponent<WritesRegistryTarget>(entity);
    registry.AddComponent<ReadsRegistryTarget>(entity);
    registry.AddComponent<RegistryTarget>(entity);
    const ComponentSchedule& before = registry.CloseRegistration();
    const std::size_t levels = before.Levels().size();
    const std::string dump = registry.Describe();

    registry.Clear();

    EXPECT_EQ(registry.EntityCount(), 0u);
    EXPECT_EQ(registry.Count<WritesRegistryTarget>(), 0u);
    // A declaration is a property of the type, not of the entity population.
    EXPECT_EQ(registry.Schedule().Levels().size(), levels);
    EXPECT_EQ(registry.Describe(), dump);
    // Re-adding the same types still works: the registration is not a concept.
    Entity& again = registry.CreateEntity();
    registry.AddComponent<WritesRegistryTarget>(again);
    registry.UpdateComponents(0.016f);
}

TEST(ComponentScheduleRegistryTest, TwoRegistriesHaveIndependentSchedules) {
    ComponentRegistry first;
    Entity& a = first.CreateEntity();
    first.AddComponent<WritesRegistryTarget>(a);
    first.AddComponent<ReadsRegistryTarget>(a);

    ComponentRegistry second;
    Entity& b = second.CreateEntity();
    second.AddComponent<ReadsRegistryTarget>(b);
    second.AddComponent<WritesRegistryTarget>(b);

    // Registration order decides the levels, and each registry has its own.
    EXPECT_EQ(first.Schedule().Levels()[0].type_ids[0],
              ComponentTypeIDSystem::GetTypeID<WritesRegistryTarget>());
    EXPECT_EQ(second.Schedule().Levels()[0].type_ids[0],
              ComponentTypeIDSystem::GetTypeID<ReadsRegistryTarget>());
    EXPECT_NE(first.Describe(), second.Describe());
}

TEST(ComponentScheduleRegistryTest, TheScheduleIsFrozenAtTheFirstClose) {
    ComponentRegistry registry;
    Entity& entity = registry.CreateEntity();
    registry.AddComponent<WritesRegistryTarget>(entity);
    const ComponentSchedule& schedule = registry.CloseRegistration();

    // Idempotent: the second call returns the same schedule, not a re-resolution.
    const ComponentSchedule& again = registry.CloseRegistration();
    EXPECT_EQ(&schedule, &again);
    EXPECT_EQ(schedule.Levels().size(), 1u);

    // A new object type registered after the close throws, naming itself and the
    // remedy. The message is the contract: it tells an application what to do.
    Entity& other = registry.CreateEntity();
    try {
        registry.AddComponent<ReadsRegistryTarget>(other);
        FAIL() << "adding a never-registered component type after close must throw";
    } catch (const std::logic_error& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("registration closed"), std::string::npos) << message;
        EXPECT_NE(message.find("RegisterComponent"), std::string::npos) << message;
    }
}

TEST(ComponentScheduleRegistryTest, ExplicitRegistrationMakesANodeWithoutInstances) {
    ComponentRegistry registry;
    registry.RegisterComponent<ReadsRegistryTarget>();
    const ComponentSchedule& schedule = registry.CloseRegistration();
    ASSERT_EQ(schedule.Levels().size(), 1u);
    EXPECT_EQ(schedule.Levels()[0].type_ids,
              (std::vector<std::size_t>{ComponentTypeIDSystem::GetTypeID<ReadsRegistryTarget>()}));

    // Registered but empty: zero units, so the plan has no work for it.
    const std::vector<PlanSegment> plan = registry.ExecutionPlan();
    ASSERT_EQ(plan.size(), 1u);
    EXPECT_FALSE(plan[0].parallel);
}

TEST(ComponentScheduleRegistryTest, SameRegistrationSequenceProducesTheSameDump) {
    auto build = [] {
        ComponentRegistry registry;
        Entity& entity = registry.CreateEntity();
        registry.AddComponent<WritesRegistryTarget>(entity);
        registry.AddComponent<ReadsRegistryTarget>(entity);
        return registry.Describe();
    };
    EXPECT_EQ(build(), build());
}

// A fixture where every type and every target carries a `kName`, so the dump is
// name-based end to end and identical in every configuration of one compiler.
namespace {

struct DumpTarget : Component {
    static constexpr std::string_view kName = "test.dump_target";
};

struct DumpAlpha : Component {
    using SelfReads = AccessSet<DumpTarget>;
    using SelfWrites = AccessSet<DumpTarget>;
    static constexpr Concurrency kConcurrency = Concurrency::SelfParallel;
    static constexpr std::uint32_t kServices = ServiceRead(Service::Input);
    static constexpr std::string_view kName = "test.dump_alpha";
    void Update(float, UpdateScope&) override {}
};

struct DumpBravo : Component {
    using SelfWrites = AccessSet<DumpTarget>;
    static constexpr Concurrency kConcurrency = Concurrency::SelfParallel;
    static constexpr std::string_view kName = "test.dump_bravo";
    void Update(float, UpdateScope&) override {}
};

struct DumpCharlie : Component {
    using Any = AccessSet<DumpTarget>;
    static constexpr std::string_view kName = "test.dump_charlie";
    void Update(float, UpdateScope&) override {}
};

} // namespace

TEST(ComponentScheduleRegistryTest, TheDumpSnapshotIsNameBasedAndStable) {
    // Pin the target's type id below the declarers', so the `w:` order is a
    // property of the fixture. A read-only id fetch must not register a node.
    (void)ComponentTypeIDSystem::GetTypeID<DumpTarget>();

    ComponentRegistry registry;
    Entity& entity = registry.CreateEntity();
    registry.AddComponent<DumpAlpha>(entity);
    registry.AddComponent<DumpBravo>(entity);
    registry.AddComponent<DumpCharlie>(entity);

    // An exact snapshot, asserted identically in every configuration: every row
    // and every edge resolved a name, so nothing here depends on the compiler's
    // spelling of a type. A release build and a strict-check build must agree.
    constexpr std::string_view kExpected =
        "component schedule: 3 types, 3 levels\n"
        "level 0 (1 type):\n"
        "  test.dump_alpha          [members]  mode:self-parallel self r:test.dump_target "
        "w:test.dump_target,test.dump_alpha  services r:Input\n"
        "level 1 (1 type):\n"
        "  test.dump_bravo          [members]  mode:self-parallel self r:- "
        "w:test.dump_target,test.dump_bravo\n"
        "level 2 (1 type):\n"
        "  test.dump_charlie        [members]  mode:exclusive     self r:- "
        "w:test.dump_charlie  any:test.dump_target\n"
        "edges:\n"
        "  test.dump_alpha -> test.dump_bravo  (conflict: write/write test.dump_target, "
        "discovery order)\n"
        "  test.dump_alpha -> test.dump_charlie  (exclusive, discovery order)\n"
        "  test.dump_bravo -> test.dump_charlie  (exclusive, discovery order)\n";

    EXPECT_EQ(registry.Describe(), kExpected);
    EXPECT_EQ(registry.Describe().find("component_type["), std::string::npos);
}

TEST(ComponentScheduleTest, ContradictoryExplicitOrderingOnOnePairIsACycle) {
    // Two contradictory declarations on the same pair used to collapse to one
    // edge, so the cycle was never seen and the resolver silently honored
    // whichever was inserted first.
    {
        auto alpha = MakeInfo(0, "alpha");
        alpha.before.insert(1);
        auto bravo = MakeInfo(1, "bravo");
        bravo.before.insert(0);
        const std::vector<ComponentAccessInfo> infos{alpha, bravo};

        auto result = ComponentSchedule::Resolve(infos, {});
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().cycle, (std::vector<std::size_t>{0, 1}));
    }
    {
        // The same contradiction written as Before + After on one type.
        auto alpha = MakeInfo(0, "alpha");
        alpha.before.insert(1);
        alpha.after.insert(1);
        auto bravo = MakeInfo(1, "bravo");
        const std::vector<ComponentAccessInfo> infos{alpha, bravo};

        auto result = ComponentSchedule::Resolve(infos, {});
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().cycle, (std::vector<std::size_t>{0, 1}));
    }
}
