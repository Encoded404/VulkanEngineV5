module;

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

// Pure math for the shared scene explorer: camera angle conventions, path
// sampling and bounds. Deliberately engine-free so it can be unit-tested
// without a Vulkan device.
export module Examples.Explorer.Math;

import std;

export namespace Examples::Explorer {

// Axis-aligned world bounds of a loaded scene. `valid` is false when the scene
// had no geometry, so consumers fall back to a unit-radius frame instead of
// dividing by zero.
struct Bounds {
    glm::vec3 min{0.0f, 0.0f, 0.0f};
    glm::vec3 max{0.0f, 0.0f, 0.0f};
    bool valid = false;

    [[nodiscard]] glm::vec3 Center() const noexcept {
        return valid ? (min + max) * 0.5f : glm::vec3{0.0f};
    }

    [[nodiscard]] glm::vec3 Size() const noexcept {
        return valid ? (max - min) : glm::vec3{0.0f};
    }

    // Half-diagonal length, clamped to a usable minimum so a single point (or
    // a degenerate axis) still frames.
    [[nodiscard]] float Radius() const noexcept {
        if (!valid) return 1.0f;
        const float radius = glm::length((max - min) * 0.5f);
        return radius > 1e-5f ? radius : 1.0f;
    }
};

// One key of a camera path. The camera sits at `position` and looks at
// `target`; leave every target at the origin to have the sampler derive a
// look-ahead target from the path tangent instead.
struct PathKey {
    glm::vec3 position{0.0f, 0.0f, 0.0f};
    glm::vec3 target{0.0f, 0.0f, 0.0f};
};

// A named camera path a scene offers in Path mode. A closed path wraps the
// Catmull-Rom segment around the last key; an open path pins the duplicate end
// control points.
struct CameraPath {
    std::string name{};
    std::vector<PathKey> keys{};
    bool closed = false;

    [[nodiscard]] bool Valid() const noexcept { return keys.size() >= 2; }
};

struct PathSample {
    glm::vec3 position{0.0f, 0.0f, 0.0f};
    glm::vec3 target{0.0f, 0.0f, 0.0f};
};

// Total polyline length, sampled finely enough to convert a metres/second
// path speed into a normalized path parameter rate.
[[nodiscard]] float ApproximatePathLength(const CameraPath& path);

// `t` is normalized: [0, 1] on an open path, wrapped on a closed one.
[[nodiscard]] PathSample SamplePath(const CameraPath& path, float t);

// Freefly convention: yaw 0 looks toward -Z and positive yaw turns toward +X.
// Pitch is positive looking up. Both are degrees.
[[nodiscard]] glm::vec3 DirectionFromAngles(float yaw_degrees, float pitch_degrees) noexcept;
[[nodiscard]] glm::vec2 AnglesFromDirection(const glm::vec3& direction) noexcept;

// Orbit convention: the offset points from the orbit target to the camera.
[[nodiscard]] glm::vec3 OrbitOffsetFromAngles(float yaw_degrees, float pitch_degrees) noexcept;
[[nodiscard]] glm::vec2 OrbitAnglesFromOffset(const glm::vec3& offset) noexcept;

// Frame-rate independent smoothing: `1 - exp(-rate * dt)` interpolation.
[[nodiscard]] float Damp(float current, float target, float rate, float delta_time) noexcept;
[[nodiscard]] glm::vec3 Damp(const glm::vec3& current, const glm::vec3& target, float rate,
                             float delta_time) noexcept;

} // namespace Examples::Explorer
