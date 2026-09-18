#include <gtest/gtest.h>

import std;
import std.compat;

import Examples.InfiniteRunner.Balance;

namespace {

using Examples::InfiniteRunner::BalanceConfig;

// Golden fingerprint of the v1 default ruleset, computed with the documented
// FNV-1a-64 contract (version, then every field in declaration order, floats
// folded via their bit pattern). Update deliberately when kVersion changes.
constexpr std::uint64_t kGoldenV1 = 0xEE1A8498415DBD96ULL;

TEST(BalanceConfigTest, DefaultRulesetFingerprintIsStable) {
    EXPECT_EQ(BalanceConfig{}.Hash(), kGoldenV1);
}

TEST(BalanceConfigTest, FingerprintIsDeterministic) {
    EXPECT_EQ(BalanceConfig{}.Hash(), BalanceConfig{}.Hash());
}

// Every gameplay-affecting field must participate in the fingerprint; a field
// that silently stopped being hashed would merge two different rulesets onto
// one leaderboard.
TEST(BalanceConfigTest, EveryFieldChangesTheFingerprint) {
    const BalanceConfig base{};
    const std::uint64_t baseline = base.Hash();

    const auto changed = [&](auto mutate) {
        BalanceConfig c = base;
        mutate(c);
        return c.Hash();
    };

    EXPECT_NE(changed([](BalanceConfig& c) { c.corridor_half += 0.01f; }), baseline);
    EXPECT_NE(changed([](BalanceConfig& c) { c.wall_height += 0.01f; }), baseline);
    EXPECT_NE(changed([](BalanceConfig& c) { c.wall_depth += 0.01f; }), baseline);
    EXPECT_NE(changed([](BalanceConfig& c) { c.min_block_width += 0.001f; }), baseline);
    EXPECT_NE(changed([](BalanceConfig& c) { c.player_size += 0.01f; }), baseline);
    EXPECT_NE(changed([](BalanceConfig& c) { c.player_speed += 0.01f; }), baseline);
    EXPECT_NE(changed([](BalanceConfig& c) { c.wall_speed += 0.01f; }), baseline);
    EXPECT_NE(changed([](BalanceConfig& c) { c.wall_spawn_z -= 1.0f; }), baseline);
    EXPECT_NE(changed([](BalanceConfig& c) { c.wall_initial_spawn_z -= 1.0f; }), baseline);
    EXPECT_NE(changed([](BalanceConfig& c) { c.wall_recycle_z += 1.0f; }), baseline);
    EXPECT_NE(changed([](BalanceConfig& c) { c.wall_spacing_pow_scaling += 0.01f; }), baseline);
    EXPECT_NE(changed([](BalanceConfig& c) { c.wall_count += 1; }), baseline);
    EXPECT_NE(changed([](BalanceConfig& c) { c.wall_hole_min += 0.01f; }), baseline);
    EXPECT_NE(changed([](BalanceConfig& c) { c.wall_hole_max += 0.01f; }), baseline);
    EXPECT_NE(changed([](BalanceConfig& c) { c.wall_hole_size_pow_scaling += 0.01f; }), baseline);
    EXPECT_NE(changed([](BalanceConfig& c) { c.wall_hole_placement_min += 0.01f; }), baseline);
    EXPECT_NE(changed([](BalanceConfig& c) { c.wall_hole_placement_max += 0.01f; }), baseline);
    EXPECT_NE(changed([](BalanceConfig& c) { c.difficulty_scaling_divider += 0.1f; }), baseline);
}

TEST(BalanceConfigTest, WallSpacingIsDerivedFromFields) {
    BalanceConfig c{};
    EXPECT_FLOAT_EQ(c.WallSpacing(), (c.wall_recycle_z - c.wall_spawn_z) / static_cast<float>(c.wall_count));
}

} // namespace
