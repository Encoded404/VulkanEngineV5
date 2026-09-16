module;

module Examples.InfiniteRunner.Balance;

import std;

namespace Examples::InfiniteRunner {

namespace {

// FNV-1a 64-bit. Stable, dependency-free and easy to reimplement in a test or
// an external tool; the exact algorithm is part of the fingerprint contract.
constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime  = 1099511628211ULL;

std::uint64_t FoldU32(std::uint64_t h, std::uint32_t value) noexcept {
    for (int i = 0; i < 4; ++i) {
        h ^= static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFU);
        h *= kFnvPrime;
    }
    return h;
}

std::uint64_t FoldFloat(std::uint64_t h, float value) noexcept {
    // bit_cast rather than a numeric conversion: the fingerprint is over the
    // exact ruleset, so a value that only differs by rounding must differ here.
    return FoldU32(h, std::bit_cast<std::uint32_t>(value));
}

} // namespace

std::uint64_t BalanceConfig::Hash() const noexcept {
    std::uint64_t h = kFnvOffset;
    h = FoldU32(h, kVersion);
    h = FoldFloat(h, corridor_half);
    h = FoldFloat(h, wall_height);
    h = FoldFloat(h, wall_depth);
    h = FoldFloat(h, min_block_width);
    h = FoldFloat(h, player_size);
    h = FoldFloat(h, player_speed);
    h = FoldFloat(h, wall_speed);
    h = FoldFloat(h, wall_spawn_z);
    h = FoldFloat(h, wall_initial_spawn_z);
    h = FoldFloat(h, wall_recycle_z);
    h = FoldFloat(h, wall_spacing_pow_scaling);
    h = FoldU32(h, static_cast<std::uint32_t>(wall_count));
    h = FoldFloat(h, wall_hole_min);
    h = FoldFloat(h, wall_hole_max);
    h = FoldFloat(h, wall_hole_size_pow_scaling);
    h = FoldFloat(h, wall_hole_placement_min);
    h = FoldFloat(h, wall_hole_placement_max);
    h = FoldFloat(h, difficulty_scaling_divider);
    return h;
}

} // namespace Examples::InfiniteRunner
