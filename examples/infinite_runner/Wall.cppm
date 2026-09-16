module;

#include <glm/glm.hpp> // NOLINT(misc-include-cleaner)

export module Examples.InfiniteRunner.Wall;

import std;

export import Examples.InfiniteRunner.Sweep;

export namespace Examples::InfiniteRunner {

// ── Wall / corridor geometry ──
// Single source of truth for both the swept collision test and the render
// transforms, so the visual blocks and the hitboxes cannot drift apart.
inline constexpr float kCorridorHalf = 3.5f; // half-width of the play corridor
inline constexpr float kWallHeight = 2.25f;
inline constexpr float kWallDepth = 0.6f;
inline constexpr float kMinBlockWidth = 0.05f; // keeps a degenerate block visible

// One wall: two solid blocks leaving the gap [gap_left_x, gap_right_x] open.
// Pure state; the owning game stores the render handles separately.
struct Wall {
    float gap_left_x = 0.0f;  // max x of the left block
    float gap_right_x = 0.0f; // min x of the right block
    float z = 0.0f;
    bool passed = false;

    // Solid block spanning the corridor's left edge to the gap.
    [[nodiscard]] Sweep::Aabb LeftBlock() const noexcept {
        const float half_height = kWallHeight * 0.5f;
        const float half_depth = kWallDepth * 0.5f;
        const float right_x = std::max(gap_left_x, -kCorridorHalf + kMinBlockWidth);
        return {{-kCorridorHalf, -half_height, z - half_depth},
                {right_x, half_height, z + half_depth}};
    }

    // Solid block spanning the gap to the corridor's right edge.
    [[nodiscard]] Sweep::Aabb RightBlock() const noexcept {
        const float half_height = kWallHeight * 0.5f;
        const float half_depth = kWallDepth * 0.5f;
        const float left_x = std::min(gap_right_x, kCorridorHalf - kMinBlockWidth);
        return {{left_x, -half_height, z - half_depth},
                {kCorridorHalf, half_height, z + half_depth}};
    }
};

} // namespace Examples::InfiniteRunner
