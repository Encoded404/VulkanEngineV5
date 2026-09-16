module;

#include <glm/glm.hpp> // NOLINT(misc-include-cleaner)

export module Examples.InfiniteRunner.Wall;

import std;

export import Examples.InfiniteRunner.Sweep;
export import Examples.InfiniteRunner.Balance;

export namespace Examples::InfiniteRunner {

// One wall: two solid blocks leaving the gap [gap_left_x, gap_right_x] open.
// Pure state; the owning game stores the render handles separately.
//
// The block extents come from the BalanceConfig passed in at call time rather
// than from namespace-scope constants, so the collision geometry is always the
// same ruleset the score is reported under.
struct Wall {
    float gap_left_x = 0.0f;  // max x of the left block
    float gap_right_x = 0.0f; // min x of the right block
    float z = 0.0f;
    bool passed = false;

    // Solid block spanning the corridor's left edge to the gap.
    [[nodiscard]] Sweep::Aabb LeftBlock(const BalanceConfig& balance) const noexcept {
        const float half_height = balance.wall_height * 0.5f;
        const float half_depth = balance.wall_depth * 0.5f;
        const float right_x = std::max(gap_left_x, -balance.corridor_half + balance.min_block_width);
        return {{-balance.corridor_half, -half_height, z - half_depth},
                {right_x, half_height, z + half_depth}};
    }

    // Solid block spanning the gap to the corridor's right edge.
    [[nodiscard]] Sweep::Aabb RightBlock(const BalanceConfig& balance) const noexcept {
        const float half_height = balance.wall_height * 0.5f;
        const float half_depth = balance.wall_depth * 0.5f;
        const float left_x = std::min(gap_right_x, balance.corridor_half - balance.min_block_width);
        return {{left_x, -half_height, z - half_depth},
                {balance.corridor_half, half_height, z + half_depth}};
    }
};

} // namespace Examples::InfiniteRunner
