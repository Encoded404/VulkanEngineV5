module;

export module Examples.InfiniteRunner.Balance;

import std;

export namespace Examples::InfiniteRunner {

// Gameplay-affecting tuning for the infinite runner.
//
// This is the single source of truth for every value that changes the
// simulation outcome (corridor geometry, player/wall motion, gap generation and
// the difficulty ramp). Cosmetic settings such as camera framing, lighting and
// material colours deliberately stay in the game itself.
//
// The whole set is fingerprinted by Hash(); leaderboards are separated per
// fingerprint, so bump kVersion whenever a value or the field order changes.
struct BalanceConfig {
    static constexpr std::uint32_t kVersion = 1;

    // ── Corridor geometry ──
    // Shared by the swept collision test and the render transforms so the
    // visible blocks and the hitboxes cannot drift apart.
    float corridor_half   = 3.5f;
    float wall_height     = 2.25f;
    float wall_depth      = 0.6f;
    float min_block_width = 0.05f; // keeps a degenerate block visible

    // ── Player ──
    float player_size  = 0.9f;
    float player_speed = 9.0f; // units/second sideways

    // ── Wall streaming ──
    float wall_speed               = 15.0f;  // units/second toward the player
    float wall_spawn_z             = -150.0f; // far end (camera looks toward -Z)
    float wall_initial_spawn_z     = -10.0f;
    float wall_recycle_z           = 8.0f;   // once a wall passes this, wrap it back
    float wall_spacing_pow_scaling = 0.75f;
    int   wall_count               = 8;

    // ── Gap generation ──
    float wall_hole_min              = 1.0f;
    float wall_hole_max              = 1.2f;
    float wall_hole_size_pow_scaling = 0.08f;
    float wall_hole_placement_min    = 0.8f;
    float wall_hole_placement_max    = 3.5f;

    // ── Difficulty ramp ──
    float difficulty_scaling_divider = 10.0f;

    // Distance between adjacent walls. Derived from the fields above so only
    // the sources are hashed, never the derivative.
    [[nodiscard]] float WallSpacing() const noexcept {
        return (wall_recycle_z - wall_spawn_z) / static_cast<float>(wall_count);
    }

    // Runtime fingerprint of this ruleset.
    //
    // Defined out-of-line (see Balance.cpp) on purpose: a constexpr/inline
    // definition would let the compiler fold a default-constructed config to a
    // literal constant, which decouples the reported hash from the actual
    // in-memory values. It is still only a ruleset label, not an anti-tamper
    // mechanism — a client can always report whatever it likes.
    [[nodiscard]] std::uint64_t Hash() const noexcept;
};

} // namespace Examples::InfiniteRunner
