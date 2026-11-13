// The component update pass: lifecycle, guards, validation and concurrency.
//
// These are the runtime halves of the component-access contract: what the
// executor guarantees (levels run in order, units within a level overlap only as
// declared), what the capability guarantees (the scope is the only registry
// path while a pass is active), and what the validator reports when a component
// touches something it did not declare.
//
// Validation tests are skipped in a build where the checks are compiled out
// (NDEBUG without VKENGINE_COMPONENT_CHECKS). The schedule and guard tests are
// not: they must hold in every configuration.

#include <gtest/gtest.h>
#include <logging/logging_macros.hpp>  // LOGIFACE_LOG macro only — safe with import

import std;
import logiface;

import VulkanEngine.ECS.ComponentAccess;
import VulkanEngine.ECS.ComponentContext;
import VulkanEngine.ECS.ComponentRegistry;
import VulkanEngine.ECS.ComponentSchedule;
import VulkanEngine.Input;
import VulkanShared.Jobs;
import VulkanShared.SystemProfile;

namespace {

using namespace VulkanEngine;
using namespace std::chrono_literals;

// ── Capturing logger ──
//
// The LogOnce path has to be asserted, not assumed: a violation reported by two
// workers at once must produce exactly one line.
class CaptureLogger final : public Logiface::Logger {
public:
    explicit CaptureLogger(Logiface::Level level = Logiface::Level::trace) : level_(level) {}

    void Log(const Logiface::Record& record) override {
        if (record.lvl >= level_) lines_.push_back(record.message);
    }
    void SetLevel(Logiface::Level level) noexcept override { level_ = level; }
    [[nodiscard]] Logiface::Level GetLevel() const noexcept override { return level_; }

    [[nodiscard]] std::size_t Count(std::string_view needle) const {
        std::size_t count = 0;
        for (const std::string& line : lines_) {
            if (line.find(needle) != std::string::npos) ++count;
        }
        return count;
    }

    std::vector<std::string> lines_;

private:
    Logiface::Level level_;
};

class ScopedLogger {
public:
    explicit ScopedLogger(Logiface::Logger& logger) : previous_(Logiface::GetLogger()) {
        Logiface::SetLogger(&logger);
    }
    ~ScopedLogger() { Logiface::SetLogger(previous_); }
    ScopedLogger(const ScopedLogger&) = delete;
    ScopedLogger& operator=(const ScopedLogger&) = delete;

private:
    Logiface::Logger* previous_;
};

// ── Shared step recorder ──
//
// Static because the registry constructs components itself. Reset per test.
struct StepLog {
    static void Reset() {
        const std::scoped_lock lock(mutex);
        entries.clear();
        inside.store(0);
        max_inside.store(0);
    }

    static void Add(std::string_view entry) {
        const std::scoped_lock lock(mutex);
        entries.emplace_back(entry);
    }

    static void Enter() {
        const int value = inside.fetch_add(1) + 1;
        int observed = max_inside.load();
        while (value > observed && !max_inside.compare_exchange_weak(observed, value)) {
        }
    }
    static void Leave() { inside.fetch_sub(1); }

    static inline std::mutex mutex{};
    static inline std::vector<std::string> entries{};
    static inline std::atomic<int> inside{0};
    static inline std::atomic<int> max_inside{0};
};

// ── Components ──

// Three exclusive types: three levels, one merged sequential segment.
class OrderAlpha : public Component {
public:
    static constexpr std::string_view kName = "test.order_alpha";
    void Update(float, UpdateScope&) override { StepLog::Add("alpha"); }
};
class OrderBravo : public Component {
public:
    static constexpr std::string_view kName = "test.order_bravo";
    void Update(float, UpdateScope&) override { StepLog::Add("bravo"); }
};
class OrderCharlie : public Component {
public:
    static constexpr std::string_view kName = "test.order_charlie";
    void Update(float, UpdateScope&) override { StepLog::Add("charlie"); }
};

// One configure hook and one update, in that order, exactly once.
class ConfigureProbe : public Component {
public:
    static constexpr std::string_view kName = "test.configure_probe";
    static constexpr std::uint32_t kServices = ServiceRead(Service::Input);

    void OnConfigure(const ComponentContext& context) override {
        StepLog::Add("configure");
        ++configure_calls;
        bound_ = context.Input() != nullptr;
    }
    void Update(float, UpdateScope&) override {
        StepLog::Add("update");
        ++update_calls;
        ++updates;
    }

    static inline std::atomic<int> configure_calls{0};
    static inline std::atomic<int> update_calls{0};
    bool bound_ = false;
    int updates = 0;
};

class ThrowingConfigure : public Component {
public:
    static constexpr std::string_view kName = "test.throwing_configure";
    void OnConfigure(const ComponentContext&) override {
        ++calls;
        if (calls == 1) throw std::runtime_error("configure failed");
    }
    static inline int calls = 0;
};

// A component that reaches back into the registry it was given. Used to prove
// every entry point throws while a pass owns the registry.
class RegistryReacher : public Component {
public:
    explicit RegistryReacher(ComponentRegistry* registry) : registry_(registry) {}
    static constexpr std::string_view kName = "test.registry_reacher";

    void OnConfigure(const ComponentContext&) override {
        if (configure_action) configure_action(*registry_);
    }
    void Update(float, UpdateScope&) override {
        if (update_action) update_action(*registry_);
    }

    std::function<void(ComponentRegistry&)> update_action;
    std::function<void(ComponentRegistry&)> configure_action;

private:
    ComponentRegistry* registry_;
};

class ThrowingAttach : public Component {
public:
    static constexpr std::string_view kName = "test.throwing_attach";
    void OnAttach(Entity&) override {
        ++attach_calls;
        throw std::runtime_error("attach failed");
    }
    void OnConfigure(const ComponentContext&) override { ++configure_calls; }

    static inline int attach_calls = 0;
    static inline int configure_calls = 0;
};

class ThrowingUpdate : public Component {
public:
    static constexpr std::string_view kName = "test.throwing_update";
    void Update(float, UpdateScope&) override { throw std::runtime_error("update failed"); }
};

// Two SelfParallel instances that must meet: each signals and then waits for
// the other. A serialized pair times out instead of spinning.
struct OverlapGate {
    static void Reset() {
        first = std::promise<void>{};
        second = std::promise<void>{};
        first_future = first.get_future().share();
        second_future = second.get_future().share();
        overlaps.store(0);
    }

    static inline std::promise<void> first{};
    static inline std::promise<void> second{};
    static inline std::shared_future<void> first_future{};
    static inline std::shared_future<void> second_future{};
    static inline std::atomic<int> overlaps{0};
};

class OverlapProbe : public Component {
public:
    explicit OverlapProbe(int index) : index_(index) {}
    static constexpr Concurrency kConcurrency = Concurrency::SelfParallel;
    static constexpr std::string_view kName = "test.overlap_probe";

    void Update(float, UpdateScope&) override {
        if (index_ == 0) {
            OverlapGate::first.set_value();
            if (OverlapGate::second_future.wait_for(2s) == std::future_status::ready) {
                OverlapGate::overlaps.fetch_add(1);
            }
        } else {
            OverlapGate::second.set_value();
            if (OverlapGate::first_future.wait_for(2s) == std::future_status::ready) {
                OverlapGate::overlaps.fetch_add(1);
            }
        }
    }

private:
    int index_;
};

// Exclusive (default): its own instances are one many-instance unit, so they
// never overlap each other even on two workers.
class ExclusiveProbe : public Component {
public:
    static constexpr std::string_view kName = "test.exclusive_probe";
    void Update(float, UpdateScope&) override {
        StepLog::Enter();
        StepLog::Add("exclusive");
        StepLog::Leave();
    }
};

// Two Serial types with no shared access: different types overlap.
class CrossAlpha : public Component {
public:
    static constexpr Concurrency kConcurrency = Concurrency::Serial;
    static constexpr std::string_view kName = "test.cross_alpha";
    void Update(float, UpdateScope&) override {
        StepLog::Enter();
        std::this_thread::sleep_for(40ms);
        StepLog::Leave();
    }
};
class CrossBravo : public Component {
public:
    static constexpr Concurrency kConcurrency = Concurrency::Serial;
    static constexpr std::string_view kName = "test.cross_bravo";
    void Update(float, UpdateScope&) override {
        StepLog::Enter();
        std::this_thread::sleep_for(40ms);
        StepLog::Leave();
    }
};

// Serial types that only read the input service do not conflict.
class InputReaderAlpha : public Component {
public:
    static constexpr Concurrency kConcurrency = Concurrency::Serial;
    static constexpr std::uint32_t kServices = ServiceRead(Service::Input);
    static constexpr std::string_view kName = "test.input_reader_alpha";
    void Update(float, UpdateScope& scope) override {
        StepLog::Enter();
        (void)scope.Services().InputRead();
        std::this_thread::sleep_for(40ms);
        StepLog::Leave();
    }
};
class InputReaderBravo : public Component {
public:
    static constexpr Concurrency kConcurrency = Concurrency::Serial;
    static constexpr std::uint32_t kServices = ServiceRead(Service::Input);
    static constexpr std::string_view kName = "test.input_reader_bravo";
    void Update(float, UpdateScope& scope) override {
        StepLog::Enter();
        (void)scope.Services().InputRead();
        std::this_thread::sleep_for(40ms);
        StepLog::Leave();
    }
};
class InputWriter : public Component {
public:
    static constexpr Concurrency kConcurrency = Concurrency::Serial;
    static constexpr std::uint32_t kServices =
        ServiceRead(Service::Input) | ServiceWrite(Service::Input);
    static constexpr std::string_view kName = "test.input_writer";
    void Update(float, UpdateScope& scope) override {
        StepLog::Enter();
        (void)scope.Services().Input();
        std::this_thread::sleep_for(40ms);
        StepLog::Leave();
    }
};

// Queues a destroy of another entity from inside Update.
class Destroyer : public Component {
public:
    static constexpr Concurrency kConcurrency = Concurrency::SelfParallel;
    static constexpr std::string_view kName = "test.destroyer";

    EntityId victim{};
    void Update(float, UpdateScope& scope) override { scope.Destroy(victim); }
};

// ── Validation fixtures ──

struct ViolationTarget : Component {
    static constexpr std::string_view kName = "test.violation_target";
};

class UndeclaredSelfReader : public Component {
public:
    static constexpr std::string_view kName = "test.undeclared_self_reader";
    void Update(float, UpdateScope& scope) override { (void)scope.Read<ViolationTarget>(); }
};

class UndeclaredSelfWriter : public Component {
public:
    static constexpr std::string_view kName = "test.undeclared_self_writer";
    void Update(float, UpdateScope& scope) override { (void)scope.Find<ViolationTarget>(); }
};

class UndeclaredCrossEntity : public Component {
public:
    static constexpr std::string_view kName = "test.undeclared_cross_entity";
    EntityId other{};
    void Update(float, UpdateScope& scope) override { (void)scope.Read<ViolationTarget>(other); }
};

class UndeclaredForEach : public Component {
public:
    static constexpr std::string_view kName = "test.undeclared_foreach";
    void Update(float, UpdateScope& scope) override {
        scope.ForEach<ViolationTarget>([](ViolationTarget&) {});
    }
};

class UndeclaredServiceWriter : public Component {
public:
    static constexpr std::uint32_t kServices = ServiceRead(Service::Input);
    static constexpr std::string_view kName = "test.undeclared_service_writer";
    void Update(float, UpdateScope& scope) override { (void)scope.Services().Input(); }
};

// SelfParallel, so two instances can be in flight at once: the dedup test needs
// two workers violating the same key concurrently.
class ConcurrentUndeclaredReader : public Component {
public:
    static constexpr Concurrency kConcurrency = Concurrency::SelfParallel;
    static constexpr std::string_view kName = "test.concurrent_undeclared_reader";
    void Update(float, UpdateScope& scope) override { (void)scope.Read<ViolationTarget>(); }
};

// Declares only the input read bit but calls the write accessor from
// OnConfigure (legal: no level is in flight) and from Update (a violation).
class ConfigureWriter : public Component {
public:
    static constexpr std::uint32_t kServices = ServiceRead(Service::Input);
    static constexpr std::string_view kName = "test.configure_writer";

    void OnConfigure(const ComponentContext& context) override {
        wrote_in_configure = context.Input() != nullptr;
    }
    void Update(float, UpdateScope& scope) override { (void)scope.Services().Input(); }

    static inline bool wrote_in_configure = false;
};

// A well-declared reader, to prove the clean path reports nothing.
class DeclaredSelfReader : public Component {
public:
    using SelfReads = AccessSet<ViolationTarget>;
    static constexpr Concurrency kConcurrency = Concurrency::Serial;
    static constexpr std::string_view kName = "test.declared_self_reader";
    void Update(float, UpdateScope& scope) override {
        observed_ = scope.Read<ViolationTarget>();
        ++calls;
    }
    static inline std::atomic<int> calls{0};
    const ViolationTarget* observed_ = nullptr;
};

[[nodiscard]] VulkanShared::JobScheduler MakeScheduler(unsigned int workers) {
    return VulkanShared::JobScheduler{VulkanShared::JobBudget{.workers = workers}};
}

} // namespace

// ── Execution order ──

TEST(ComponentUpdateTest, LevelsRunInRegistrationOrderEvenWhenMergedIntoOneSegment) {
    StepLog::Reset();
    ComponentRegistry registry;
    for (int i = 0; i < 1; ++i) {
        Entity& entity = registry.CreateEntity();
        registry.AddComponent<OrderAlpha>(entity);
        registry.AddComponent<OrderBravo>(entity);
        registry.AddComponent<OrderCharlie>(entity);
    }

    // All three are exclusive, so their units are one each: the plan merges the
    // three levels into one sequential segment.
    const std::vector<PlanSegment> plan = registry.ExecutionPlan();
    ASSERT_EQ(plan.size(), 1u);
    EXPECT_EQ(plan[0].level_count, 3u);
    EXPECT_FALSE(plan[0].parallel);

    registry.UpdateComponents(0.016f);
    EXPECT_EQ(StepLog::entries, (std::vector<std::string>{"alpha", "bravo", "charlie"}));
}

TEST(ComponentUpdateTest, OnConfigureRunsOncePerInstanceBeforeTheFirstUpdate) {
    StepLog::Reset();
    ConfigureProbe::configure_calls = 0;
    ConfigureProbe::update_calls = 0;

    ComponentRegistry registry;
    Entity& entity = registry.CreateEntity();
    registry.AddComponent<ConfigureProbe>(entity);
    registry.UpdateComponents(0.016f);
    registry.UpdateComponents(0.016f);

    EXPECT_EQ(ConfigureProbe::configure_calls.load(), 1);
    EXPECT_EQ(ConfigureProbe::update_calls.load(), 2);
    ASSERT_GE(StepLog::entries.size(), 3u);
    EXPECT_EQ(StepLog::entries[0], "configure");
    EXPECT_EQ(StepLog::entries[1], "update");
    EXPECT_EQ(StepLog::entries[2], "update");
}

TEST(ComponentUpdateTest, AThrowingConfigureLeavesTheComponentUnconfigured) {
    ThrowingConfigure::calls = 0;
    ComponentRegistry registry;
    Entity& entity = registry.CreateEntity();
    registry.AddComponent<ThrowingConfigure>(entity);

    EXPECT_THROW(registry.UpdateComponents(0.016f), std::runtime_error);
    // The flag was not set, so the next pass retries and succeeds.
    EXPECT_NO_THROW(registry.UpdateComponents(0.016f));
    EXPECT_EQ(ThrowingConfigure::calls, 2);
}

TEST(ComponentUpdateTest, AThrowingAttachStillLeavesTheInstanceToBeConfigured) {
    ThrowingAttach::attach_calls = 0;
    ThrowingAttach::configure_calls = 0;
    ComponentRegistry registry;
    Entity& entity = registry.CreateEntity();

    EXPECT_THROW(registry.AddComponent<ThrowingAttach>(entity), std::runtime_error);
    // The row is deliberately visible after a throwing hook, so the component
    // still exists and must still be configured by the next pass.
    EXPECT_TRUE(registry.HasComponent<ThrowingAttach>(entity));

    EXPECT_NO_THROW(registry.UpdateComponents(0.016f));
    EXPECT_EQ(ThrowingAttach::configure_calls, 1);
}

TEST(ComponentUpdateTest, AThrowingUpdateLeavesTheRegistryUsable) {
    ComponentRegistry registry;
    Entity& entity = registry.CreateEntity();
    registry.AddComponent<ThrowingUpdate>(entity);
    // Registered before the freeze, because the point of the test is that a
    // later add of a *known* type still works.
    registry.RegisterComponent<OrderAlpha>();

    EXPECT_THROW(registry.UpdateComponents(0.016f), std::runtime_error);

    // The update-active guard cleared on the exceptional path: all of these
    // would throw or deadlock if it had not.
    registry.RemoveComponent<ThrowingUpdate>(entity);
    registry.ApplyStructuralChanges();
    Entity& fresh = registry.CreateEntity();
    registry.AddComponent<OrderAlpha>(fresh);
    registry.ApplyStructuralChanges();
    EXPECT_NO_THROW(registry.UpdateComponents(0.016f));
    EXPECT_EQ(registry.Count<OrderAlpha>(), 1u);
}

// ── Guards ──

TEST(ComponentUpdateTest, EveryRegistryEntryPointThrowsWhileAnUpdateIsActive) {
    ComponentRegistry registry;
    Entity& owner = registry.CreateEntity();
    Entity& other = registry.CreateEntity();
    registry.AddComponent<OrderAlpha>(owner);
    registry.AddComponent<OrderBravo>(owner);
    auto& reacher = registry.AddComponent<RegistryReacher>(owner, &registry);

    const std::vector<std::pair<std::string, std::function<void(ComponentRegistry&)>>> cases{
        {"GetComponent", [&](ComponentRegistry& r) { (void)r.GetComponent<OrderAlpha>(owner); }},
        {"HasComponent", [&](ComponentRegistry& r) { (void)r.HasComponent<OrderAlpha>(owner); }},
        {"Count", [&](ComponentRegistry& r) { (void)r.Count<OrderAlpha>(); }},
        {"CreateEntity", [](ComponentRegistry& r) { (void)r.CreateEntity(); }},
        {"AddComponent",
         [&](ComponentRegistry& r) { (void)r.AddComponent<OrderAlpha>(other); }},
        {"DestroyEntity",
         [&](ComponentRegistry& r) { r.DestroyEntity(other); }},
        {"DestroyEntity(id)",
         [&](ComponentRegistry& r) { r.DestroyEntity(other.GetId()); }},
        {"RemoveComponent", [&](ComponentRegistry& r) { r.RemoveComponent<OrderAlpha>(owner); }},
        {"ForEach",
         [&](ComponentRegistry& r) { r.ForEach<OrderAlpha>([](OrderAlpha&) {}); }},
        {"ForEachEntity", [&](ComponentRegistry& r) { r.ForEachEntity([](Entity&) {}); }},
        {"TryGetEntity", [&](ComponentRegistry& r) { (void)r.TryGetEntity(other.GetId()); }},
        {"IsAlive", [&](ComponentRegistry& r) { (void)r.IsAlive(other.GetId()); }},
        {"EntityCount", [](ComponentRegistry& r) { (void)r.EntityCount(); }},
        {"OnAdd", [&](ComponentRegistry& r) { r.OnAdd<OrderAlpha>([](Entity&, OrderAlpha&) {}); }},
        {"OnRemove",
         [&](ComponentRegistry& r) { r.OnRemove<OrderAlpha>([](Entity&, OrderAlpha&) {}); }},
        {"RegisterComponent", [](ComponentRegistry& r) { r.RegisterComponent<OrderCharlie>(); }},
        {"CloseRegistration", [](ComponentRegistry& r) { (void)r.CloseRegistration(); }},
        {"Schedule", [](ComponentRegistry& r) { (void)r.Schedule(); }},
        {"AccessInfo", [](ComponentRegistry& r) { (void)r.AccessInfo(0); }},
        {"TargetNames", [](ComponentRegistry& r) { (void)r.TargetNames(); }},
        {"ExecutionPlan", [](ComponentRegistry& r) { (void)r.ExecutionPlan(); }},
        {"Describe", [](ComponentRegistry& r) { (void)r.Describe(); }},
        {"SetAccessOptions",
         [](ComponentRegistry& r) { r.SetAccessOptions(ComponentAccessOptions{}); }},
        {"ApplyStructuralChanges", [](ComponentRegistry& r) { r.ApplyStructuralChanges(); }},
        {"Clear", [](ComponentRegistry& r) { r.Clear(); }},
        {"UpdateComponents", [](ComponentRegistry& r) { r.UpdateComponents(0.016f); }},
    };

    // The same holds from the configure hook, which is also a pass. It has to
    // run before any other pass, because a configured component is never
    // configured again.
    reacher.configure_action = [](ComponentRegistry& r) {
        r.OnAdd<OrderAlpha>([](Entity&, OrderAlpha&) {});
    };
    EXPECT_THROW(registry.UpdateComponents(0.016f), std::logic_error);
    reacher.configure_action = nullptr;

    for (const auto& [name, action] : cases) {
        StepLog::Reset();
        reacher.update_action = action;
        bool threw_logic_error = false;
        try {
            registry.UpdateComponents(0.016f);
        } catch (const std::logic_error&) {
            threw_logic_error = true;
        } catch (...) {
            ADD_FAILURE() << "registry." << name << " threw something other than logic_error";
            threw_logic_error = true;
        }
        EXPECT_TRUE(threw_logic_error)
            << "registry." << name << " should be rejected during an update";
    }
    reacher.update_action = nullptr;

}

TEST(ComponentUpdateTest, ASecondThreadCannotMutateTheRegistryMidUpdate) {
    ComponentRegistry registry;
    Entity& owner = registry.CreateEntity();
    registry.AddComponent<OrderAlpha>(owner);
    auto& reacher = registry.AddComponent<RegistryReacher>(owner, &registry);

    std::atomic<int> rejections{0};
    reacher.update_action = [&](ComponentRegistry& r) {
        // A second thread, with the pass active on this one.
        std::thread intruder{[&] {
            try {
                r.ApplyStructuralChanges();
            } catch (const std::logic_error&) {
                rejections.fetch_add(1);
            }
            try {
                r.UpdateComponents(0.016f);
            } catch (const std::logic_error&) {
                rejections.fetch_add(1);
            }
            try {
                r.Clear();
            } catch (const std::logic_error&) {
                rejections.fetch_add(1);
            }
            // The mutex-taking readers re-check under the lock, so a thread that
            // passed the pre-lock early-out is still rejected.
            try {
                r.ForEach<OrderAlpha>([](OrderAlpha&) {});
            } catch (const std::logic_error&) {
                rejections.fetch_add(1);
            }
            try {
                r.ForEachEntity([](Entity&) {});
            } catch (const std::logic_error&) {
                rejections.fetch_add(1);
            }
        }};
        intruder.join();
    };

    EXPECT_NO_THROW(registry.UpdateComponents(0.016f));
    EXPECT_EQ(rejections.load(), 5);
    reacher.update_action = nullptr;
}

TEST(ComponentUpdateTest, SpawningDuringForEachThrowsInEveryConfiguration) {
    ComponentRegistry registry;
    Entity& entity = registry.CreateEntity();
    registry.AddComponent<OrderAlpha>(entity);

    EXPECT_THROW(
        registry.ForEach<OrderAlpha>([&](OrderAlpha&) { (void)registry.CreateEntity(); }),
        std::logic_error);
    EXPECT_THROW(
        registry.ForEach<OrderAlpha>([&](OrderAlpha&) { (void)registry.AddComponent<OrderBravo>(entity); }),
        std::logic_error);

    // Destroying inside a query still queues rather than mutating.
    registry.ForEach<OrderAlpha>([&](OrderAlpha&) { registry.DestroyEntity(entity); });
    EXPECT_EQ(registry.EntityCount(), 1u);
    registry.ApplyStructuralChanges();
    EXPECT_EQ(registry.EntityCount(), 0u);
}

TEST(ComponentUpdateTest, ScopeDestroyQueuesAndAppliesAtTheDrain) {
    ComponentRegistry registry;
    Entity& alpha = registry.CreateEntity();
    Entity& bravo = registry.CreateEntity();
    const EntityId alpha_id = alpha.GetId();
    const EntityId bravo_id = bravo.GetId();
    registry.RegisterComponent<OrderAlpha>();

    auto& first = registry.AddComponent<Destroyer>(alpha);
    auto& second = registry.AddComponent<Destroyer>(bravo);
    first.victim = bravo_id;
    second.victim = alpha_id;

    registry.UpdateComponents(0.016f);
    // Nothing is freed while a level is running.
    EXPECT_TRUE(registry.IsAlive(alpha_id));
    EXPECT_TRUE(registry.IsAlive(bravo_id));

    registry.ApplyStructuralChanges();
    EXPECT_FALSE(registry.IsAlive(alpha_id));
    EXPECT_FALSE(registry.IsAlive(bravo_id));
    EXPECT_EQ(registry.EntityCount(), 0u);
    // The registry is still usable after both queued destroys applied.
    Entity& fresh = registry.CreateEntity();
    registry.AddComponent<OrderAlpha>(fresh);
    EXPECT_NO_THROW(registry.UpdateComponents(0.016f));
}

// ── Concurrency ──

TEST(ComponentUpdateTest, TwoSelfParallelInstancesOverlapOnTwoWorkers) {
    OverlapGate::Reset();
    StepLog::Reset();
    auto scheduler = MakeScheduler(2);
    ComponentRegistry registry{&scheduler};

    Entity& first = registry.CreateEntity();
    Entity& second = registry.CreateEntity();
    registry.AddComponent<OverlapProbe>(first, 0);
    registry.AddComponent<OverlapProbe>(second, 1);

    registry.UpdateComponents(0.016f);
    EXPECT_EQ(OverlapGate::overlaps.load(), 2)
        << "both instances must observe the other inside its own Update";
}

TEST(ComponentUpdateTest, ExclusiveInstancesNeverOverlapEvenOnTwoWorkers) {
    StepLog::Reset();
    auto scheduler = MakeScheduler(2);
    ComponentRegistry registry{&scheduler};

    for (int i = 0; i < 4; ++i) {
        Entity& entity = registry.CreateEntity();
        registry.AddComponent<ExclusiveProbe>(entity);
    }

    registry.UpdateComponents(0.016f);
    EXPECT_EQ(StepLog::max_inside.load(), 1);
    EXPECT_EQ(StepLog::entries.size(), 4u);
}

TEST(ComponentUpdateTest, DifferentSerialTypesOverlapOnTwoWorkers) {
    StepLog::Reset();
    auto scheduler = MakeScheduler(2);
    ComponentRegistry registry{&scheduler};

    Entity& entity = registry.CreateEntity();
    registry.AddComponent<CrossAlpha>(entity);
    registry.AddComponent<CrossBravo>(entity);

    // No declared access overlap and neither is exclusive: same level.
    ASSERT_EQ(registry.Schedule().Levels().size(), 1u);
    registry.UpdateComponents(0.016f);
    EXPECT_EQ(StepLog::max_inside.load(), 2);
}

TEST(ComponentUpdateTest, ServiceReadersOverlapAndAWriterSeparatesThem) {
    StepLog::Reset();
    auto scheduler = MakeScheduler(2);

    {
        ComponentRegistry registry{&scheduler};
        Entity& entity = registry.CreateEntity();
        registry.AddComponent<InputReaderAlpha>(entity);
        registry.AddComponent<InputReaderBravo>(entity);
        ASSERT_EQ(registry.Schedule().Levels().size(), 1u);
        registry.UpdateComponents(0.016f);
        EXPECT_EQ(StepLog::max_inside.load(), 2);
    }

    StepLog::Reset();
    ComponentRegistry registry{&scheduler};
    Entity& entity = registry.CreateEntity();
    registry.AddComponent<InputReaderAlpha>(entity);
    registry.AddComponent<InputWriter>(entity);
    // A service write conflicts with any access to the same service.
    EXPECT_EQ(registry.Schedule().Levels().size(), 2u);
    registry.UpdateComponents(0.016f);
    EXPECT_EQ(StepLog::max_inside.load(), 1);
}

// ── Validation ──

TEST(ComponentUpdateTest, UndeclaredAccessThrowsWhenThePolicyIsThrow) {
    // The body is compiled in every configuration so the fixtures cannot rot;
    // only the run is skipped where the validator does not exist.
    if (!kAccessChecksCompiled) {
        GTEST_SKIP() << "validation is compiled out in this configuration";
        return;
    }
    auto expect_violation = [](auto build) {
        ComponentRegistry registry;
        registry.SetAccessOptions(
            ComponentAccessOptions{.checks = true, .on_violation = ViolationAction::Throw});
        Entity& entity = registry.CreateEntity();
        Entity& other = registry.CreateEntity();
        build(registry, entity, other);
        EXPECT_THROW(registry.UpdateComponents(0.016f), std::logic_error);
    };

    expect_violation([](ComponentRegistry& r, Entity& e, Entity&) {
        r.AddComponent<UndeclaredSelfReader>(e);
    });
    expect_violation([](ComponentRegistry& r, Entity& e, Entity&) {
        r.AddComponent<UndeclaredSelfWriter>(e);
    });
    expect_violation([](ComponentRegistry& r, Entity& e, Entity&) {
        r.AddComponent<UndeclaredForEach>(e);
    });
    expect_violation([](ComponentRegistry& r, Entity& e, Entity&) {
        r.AddComponent<UndeclaredServiceWriter>(e);
    });
    expect_violation([](ComponentRegistry& r, Entity& e, Entity& other) {
        auto& component = r.AddComponent<UndeclaredCrossEntity>(e);
        component.other = other.GetId();
    });
}

TEST(ComponentUpdateTest, AViolationIsLoggedOnceEvenFromTwoWorkers) {
    if (!kAccessChecksCompiled) {
        GTEST_SKIP() << "validation is compiled out in this configuration";
        return;
    }
    CaptureLogger logger{Logiface::Level::warn};
    const ScopedLogger installed{logger};

    auto scheduler = MakeScheduler(2);
    ComponentRegistry registry{&scheduler};
    registry.SetAccessOptions(
        ComponentAccessOptions{.checks = true, .on_violation = ViolationAction::LogOnce});

    for (int i = 0; i < 2; ++i) {
        Entity& entity = registry.CreateEntity();
        registry.AddComponent<ConcurrentUndeclaredReader>(entity);
    }

    registry.UpdateComponents(0.016f);
    EXPECT_EQ(logger.Count("undeclared self read"), 1u)
        << "the de-duplication set is per (type, target, kind), not per report";

    // A second pass reports nothing new: the key is already recorded.
    registry.UpdateComponents(0.016f);
    EXPECT_EQ(logger.Count("undeclared self read"), 1u);
}

TEST(ComponentUpdateTest, ADeclaredReadReportsNothing) {
    DeclaredSelfReader::calls = 0;
    ComponentRegistry registry;
    Entity& entity = registry.CreateEntity();
    registry.AddComponent<DeclaredSelfReader>(entity);
    registry.AddComponent<ViolationTarget>(entity);

    CaptureLogger logger{Logiface::Level::warn};
    const ScopedLogger installed{logger};
    registry.SetAccessOptions(
        ComponentAccessOptions{.checks = true, .on_violation = ViolationAction::Throw});
    EXPECT_NO_THROW(registry.UpdateComponents(0.016f));
    EXPECT_TRUE(logger.lines_.empty());
    EXPECT_EQ(DeclaredSelfReader::calls.load(), 1);
}

TEST(ComponentUpdateTest, AccessorsOutsideAnUpdateReturnTheGrantUnvalidated) {
    Input::InputSystem input{};
    ComponentContext context{};
    context.GrantInput(&input);
    // No active scope: the validator guards the barriered path only.
    EXPECT_EQ(context.Input(), &input);
    EXPECT_EQ(context.InputRead(), &input);
}

TEST(ComponentUpdateTest, AServiceWriteInOnConfigureIsAllowedButInUpdateIsNot) {
    if (!kAccessChecksCompiled) {
        GTEST_SKIP() << "validation is compiled out in this configuration";
        return;
    }
    ConfigureWriter::wrote_in_configure = false;
    ComponentRegistry registry;
    registry.SetAccessOptions(
        ComponentAccessOptions{.checks = true, .on_violation = ViolationAction::Throw});
    Input::InputSystem input{};
    ComponentContext context{};
    context.GrantInput(&input);

    Entity& entity = registry.CreateEntity();
    registry.AddComponent<ConfigureWriter>(entity);
    // The write in Update is a violation, so the pass throws; the configure
    // write already happened legally.
    EXPECT_THROW(registry.UpdateComponents(0.016f, context), std::logic_error);
    EXPECT_TRUE(ConfigureWriter::wrote_in_configure);
}
