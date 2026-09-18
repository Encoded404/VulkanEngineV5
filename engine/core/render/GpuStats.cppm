module;

export module VulkanEngine.GpuStats;

import std;

export namespace VulkanEngine::GpuStats {

// One pipeline-statistics query result. The order matches the bits in
// Renderer::GPU_STATS_FLAGS, which is also the order vkGetQueryPoolResults
// writes them.
struct Counters {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::uint64_t input_assembly_vertices = 0;
    std::uint64_t input_assembly_primitives = 0;
    std::uint64_t vertex_shader_invocations = 0;
    std::uint64_t clipping_invocations = 0;
    std::uint64_t clipping_primitives = 0;
    std::uint64_t fragment_shader_invocations = 0;
    std::uint64_t compute_shader_invocations = 0;
    // NOLINTEND(misc-non-private-member-variables-in-classes)

    Counters& operator+=(const Counters& other) {
        input_assembly_vertices += other.input_assembly_vertices;
        input_assembly_primitives += other.input_assembly_primitives;
        vertex_shader_invocations += other.vertex_shader_invocations;
        clipping_invocations += other.clipping_invocations;
        clipping_primitives += other.clipping_primitives;
        fragment_shader_invocations += other.fragment_shader_invocations;
        compute_shader_invocations += other.compute_shader_invocations;
        return *this;
    }
};

// Number of statistics counters per query (the enabled statistics bits).
inline constexpr std::uint32_t kCounterCount = 7;

// Words per query result as written by vkGetQueryPoolResults with
// VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT: the counters
// are 64-bit, the availability value is a 32-bit word that lands in the low
// half of the next 8-byte-aligned slot.
inline constexpr std::uint32_t kAvailabilityWord = kCounterCount;
inline constexpr std::uint32_t kQueryWords = kCounterCount + 1;

// A pipeline-statistics pool that enables any graphics counter can only be used
// from a graphics-capable command pool (VUID-vkCmdBeginQuery-queryType-00804).
// Compute-queue runs therefore use a separate pool that enables only the compute
// counter, which is not a graphics operation.
inline constexpr std::uint32_t kComputeCounterCount = 1;
inline constexpr std::uint32_t kComputeAvailabilityWord = kComputeCounterCount;
inline constexpr std::uint32_t kComputeQueryWords = kComputeCounterCount + 1;

[[nodiscard]] inline bool QueryAvailable(const std::uint64_t* words) {
    return words != nullptr &&
           static_cast<std::uint32_t>(words[kAvailabilityWord]) != 0u;
}

// Parses one query's words. Returns false when the query has no result yet
// (availability word zero), leaving `out` untouched.
[[nodiscard]] inline bool TryParseQueryResult(const std::uint64_t* words, Counters& out) {
    if (!QueryAvailable(words)) {
        return false;
    }
    out.input_assembly_vertices = words[0];
    out.input_assembly_primitives = words[1];
    out.vertex_shader_invocations = words[2];
    out.clipping_invocations = words[3];
    out.clipping_primitives = words[4];
    out.fragment_shader_invocations = words[5];
    out.compute_shader_invocations = words[6];
    return true;
}

// Parses one compute-pool query result (only the compute-shader counter is
// enabled in that pool).
[[nodiscard]] inline bool TryParseComputeQueryResult(const std::uint64_t* words, Counters& out) {
    if (words == nullptr || static_cast<std::uint32_t>(words[kComputeAvailabilityWord]) == 0u) {
        return false;
    }
    out.compute_shader_invocations = words[0];
    return true;
}

// Frame total from the per-run results of one frame.
[[nodiscard]] inline Counters Accumulate(std::span<const Counters> runs) {
    Counters total{};
    for (const auto& run : runs) {
        total += run;
    }
    return total;
}

// Query pool slot for a queue run. One slot per run per frames-in-flight, so a
// run of an in-flight frame is never read or reset while it is still executing.
[[nodiscard]] constexpr std::uint32_t QuerySlot(std::uint32_t fif_index,
                                                std::uint32_t run_slot,
                                                std::uint32_t frames_in_flight,
                                                std::uint32_t run_slots_per_frame) {
    return (fif_index % frames_in_flight) * run_slots_per_frame + run_slot;
}

// Total number of queries a pool must hold to track every run of every
// frames-in-flight slot.
[[nodiscard]] constexpr std::uint32_t QueryPoolSize(std::uint32_t frames_in_flight,
                                                    std::uint32_t run_slots_per_frame) {
    return frames_in_flight * run_slots_per_frame;
}

// Byte stride between consecutive query results in a vkGetQueryPoolResults read.
inline constexpr std::size_t kQueryStride =
    static_cast<std::size_t>(kQueryWords) * sizeof(std::uint64_t);
inline constexpr std::size_t kComputeQueryStride =
    static_cast<std::size_t>(kComputeQueryWords) * sizeof(std::uint64_t);

} // namespace VulkanEngine::GpuStats
