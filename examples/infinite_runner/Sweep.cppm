module;

#include <glm/glm.hpp> // NOLINT(misc-include-cleaner)

export module Examples.InfiniteRunner.Sweep;

import std;

export namespace Examples::InfiniteRunner::Sweep {

// Axis-aligned box with min/max corners. Geometry only; no game concepts.
struct Aabb {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};

    [[nodiscard]] glm::vec3 Center() const noexcept { return (min + max) * 0.5f; }
    [[nodiscard]] glm::vec3 Size() const noexcept { return max - min; }
};

// True when the segment [from, to] touches the box.
//
// A zero-length segment (from == to) degrades to a point-inside test, and a
// segment that starts inside the box counts as a hit. Both matter for the
// runner: a stationary player still needs to be hit by a wall scrolling onto
// it, and an already-overlapping player must not be able to escape by moving.
[[nodiscard]] inline bool SegmentIntersects(const Aabb& box, const glm::vec3& from, const glm::vec3& to) {
    const glm::vec3 dir = to - from;

    float t_min = 0.0f;
    float t_max = 1.0f;

    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(dir[axis]) < 1.0e-8f) {
            // Parallel to this slab: miss unless the origin is inside it.
            if (from[axis] < box.min[axis] || from[axis] > box.max[axis]) {
                return false;
            }
            continue;
        }

        const float inv = 1.0f / dir[axis];
        float t1 = (box.min[axis] - from[axis]) * inv;
        float t2 = (box.max[axis] - from[axis]) * inv;
        if (t1 > t2) {
            std::swap(t1, t2);
        }

        t_min = std::max(t_min, t1);
        t_max = std::min(t_max, t2);

        if (t_min > t_max) {
            return false;
        }
    }

    return true;
}

} // namespace Examples::InfiniteRunner::Sweep
