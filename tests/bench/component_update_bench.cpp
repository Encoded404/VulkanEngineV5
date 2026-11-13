// Device-free component-update benchmark.
//
// The ECS used to fan every object component out through one flat ParallelFor.
// The ordered executor replaces that with levels and work units, which is more
// machinery per step; this harness is what keeps that cost honest and comparable
// against the old path.
//
// It is deliberately NOT a `setup_test_target`: that helper links
// `GTest::gtest_main` and calls `gtest_discover_tests`, so the binary would
// become a ctest case and be run with `--gtest_list_tests` at build time,
// executing the timing loop on every build.
//
// Usage:  component_update_bench [--entities N] [--iterations I] [--workers W]
//
// Reporting follows the plan's measurement protocol: warmup iterations are
// discarded, the measured iterations report the median plus the 10th/90th
// percentiles, and the worker count is pinned per column with `WaitForIdle()`
// between iterations.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <vector>

import VulkanEngine.ECS.ComponentRegistry;
import VulkanShared.Jobs;

namespace {

// The type under test. It declares nothing, which is exactly the
// all-exclusive default path the executor has to be fast at: one type, one
// level, one work unit, N instances run serially inside that unit.
class BenchComponent : public VulkanEngine::Component {
public:
    void Update(float delta_time, VulkanEngine::UpdateScope& /*scope*/) override {
        // A tiny amount of work so the call is not trivially elided, without
        // dominating the dispatch cost being measured.
        accumulator += delta_time;
    }

    float accumulator = 0.0f;
};

struct Options {
    std::size_t entities = 1000;
    std::size_t iterations = 200;
    unsigned int workers = VulkanShared::JobBudget::kAuto;
};

[[nodiscard]] Options Parse(int argc, char** argv) {
    Options options;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string_view flag = argv[i];
        const char* value = argv[i + 1];
        if (flag == "--entities") options.entities = std::strtoull(value, nullptr, 10);
        else if (flag == "--iterations") options.iterations = std::strtoull(value, nullptr, 10);
        else if (flag == "--workers") {
            options.workers = static_cast<unsigned int>(std::strtoul(value, nullptr, 10));
        }
    }
    return options;
}

[[nodiscard]] double Percentile(const std::vector<double>& sorted, double fraction) {
    if (sorted.empty()) return 0.0;
    const double position = fraction * static_cast<double>(sorted.size() - 1);
    const auto index = static_cast<std::size_t>(position);
    return sorted[index];
}

} // namespace

int main(int argc, char** argv) {
    const Options options = Parse(argc, argv);

    auto& scheduler = VulkanShared::JobScheduler::Global();
    scheduler.Configure(VulkanShared::JobBudget{.workers = options.workers});

    // The registry borrows the pool explicitly, so the workers knob acts on the
    // pool the executor actually dispatches through.
    VulkanEngine::ComponentRegistry registry{&scheduler};
    for (std::size_t i = 0; i < options.entities; ++i) {
        auto& entity = registry.CreateEntity();
        registry.AddComponent<BenchComponent>(entity);
    }
    // Freezes the schedule and sizes every buffer the executor reuses, so the
    // timed loop measures steady state rather than first-use allocation.
    registry.UpdateComponents(0.016f);
    scheduler.WaitForIdle();

    constexpr std::size_t kWarmup = 100;
    for (std::size_t i = 0; i < kWarmup; ++i) {
        registry.UpdateComponents(0.016f);
        scheduler.WaitForIdle();
    }

    std::vector<double> samples;
    samples.reserve(options.iterations);
    for (std::size_t i = 0; i < options.iterations; ++i) {
        const auto start = std::chrono::steady_clock::now();
        registry.UpdateComponents(0.016f);
        scheduler.WaitForIdle();
        const auto stop = std::chrono::steady_clock::now();
        samples.push_back(std::chrono::duration<double, std::micro>(stop - start).count());
    }

    std::ranges::sort(samples);
    const double median = Percentile(samples, 0.5);
    const double p10 = Percentile(samples, 0.1);
    const double p90 = Percentile(samples, 0.9);

    std::printf("entities=%-6zu workers=%-3u iterations=%-5zu median=%9.2fus p10=%9.2fus "
                "p90=%9.2fus\n",
                options.entities, options.workers, options.iterations, median, p10, p90);
    return 0;
}
