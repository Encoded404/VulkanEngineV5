module;

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

module Examples.Explorer.Math;

import std;

namespace Examples::Explorer {

namespace {

[[nodiscard]] float ClampPitch(float pitch_degrees) noexcept {
    return std::clamp(pitch_degrees, -89.0f, 89.0f);
}

[[nodiscard]] glm::vec3 CatmullRom(const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2,
                                   const glm::vec3& p3, float t) noexcept {
    const float t2 = t * t;
    const float t3 = t2 * t;
    return 0.5f * ((2.0f * p1) + (-p0 + p2) * t +
                   (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                   (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
}

// Segment index and local parameter for a normalized path position. `segments`
// is the number of spans, which is key_count - 1 for an open path and
// key_count for a closed one.
[[nodiscard]] std::pair<int, float> LocateSegment(const CameraPath& path, float t) noexcept {
    const auto count = static_cast<int>(path.keys.size());
    const int segments = path.closed ? count : count - 1;
    if (segments <= 0) return {0, 0.0f};

    const float wrapped = path.closed ? t - std::floor(t) : std::clamp(t, 0.0f, 1.0f);
    const float scaled = wrapped * static_cast<float>(segments);
    const int segment = std::min(static_cast<int>(scaled), segments - 1);
    return {segment, scaled - static_cast<float>(segment)};
}

[[nodiscard]] std::size_t KeyIndex(const CameraPath& path, int index) noexcept {
    const auto count = static_cast<int>(path.keys.size());
    if (path.closed) {
        return static_cast<std::size_t>(((index % count) + count) % count);
    }
    return static_cast<std::size_t>(std::clamp(index, 0, count - 1));
}

[[nodiscard]] PathSample SampleRaw(const CameraPath& path, float t) noexcept {
    if (path.keys.empty()) return {};
    if (path.keys.size() == 1) return {path.keys[0].position, path.keys[0].target};

    const auto [segment, u] = LocateSegment(path, t);
    const auto i0 = KeyIndex(path, segment - 1);
    const auto i1 = KeyIndex(path, segment);
    const auto i2 = KeyIndex(path, segment + 1);
    const auto i3 = KeyIndex(path, segment + 2);

    PathSample sample{};
    sample.position = CatmullRom(path.keys[i0].position, path.keys[i1].position,
                                 path.keys[i2].position, path.keys[i3].position, u);
    sample.target = CatmullRom(path.keys[i0].target, path.keys[i1].target, path.keys[i2].target,
                               path.keys[i3].target, u);
    return sample;
}

} // namespace

glm::vec3 DirectionFromAngles(float yaw_degrees, float pitch_degrees) noexcept {
    const float yaw = glm::radians(yaw_degrees);
    const float pitch = glm::radians(pitch_degrees);
    const float cos_pitch = std::cos(pitch);
    return glm::vec3{std::sin(yaw) * cos_pitch, std::sin(pitch), -std::cos(yaw) * cos_pitch};
}

glm::vec2 AnglesFromDirection(const glm::vec3& direction) noexcept {
    if (glm::dot(direction, direction) < 1e-12f) return glm::vec2{0.0f, 0.0f};
    const glm::vec3 dir = glm::normalize(direction);
    const float yaw = glm::degrees(std::atan2(dir.x, -dir.z));
    const float pitch = glm::degrees(std::asin(std::clamp(dir.y, -1.0f, 1.0f)));
    return glm::vec2{yaw, pitch};
}

glm::vec3 OrbitOffsetFromAngles(float yaw_degrees, float pitch_degrees) noexcept {
    const float yaw = glm::radians(yaw_degrees);
    const float pitch = glm::radians(ClampPitch(pitch_degrees));
    const float cos_pitch = std::cos(pitch);
    return glm::vec3{std::sin(yaw) * cos_pitch, std::sin(pitch), std::cos(yaw) * cos_pitch};
}

glm::vec2 OrbitAnglesFromOffset(const glm::vec3& offset) noexcept {
    const float length = glm::length(offset);
    if (length < 1e-6f) return glm::vec2{0.0f, 0.0f};
    const glm::vec3 dir = offset / length;
    const float yaw = glm::degrees(std::atan2(dir.x, dir.z));
    const float pitch = glm::degrees(std::asin(std::clamp(dir.y, -1.0f, 1.0f)));
    return glm::vec2{yaw, pitch};
}

float Damp(float current, float target, float rate, float delta_time) noexcept {
    const float factor = 1.0f - std::exp(-std::max(rate, 0.0f) * std::max(delta_time, 0.0f));
    return current + (target - current) * factor;
}

glm::vec3 Damp(const glm::vec3& current, const glm::vec3& target, float rate,
               float delta_time) noexcept {
    const float factor = 1.0f - std::exp(-std::max(rate, 0.0f) * std::max(delta_time, 0.0f));
    return current + (target - current) * factor;
}

PathSample SamplePath(const CameraPath& path, float t) {
    if (!path.Valid()) return {};

    PathSample sample = SampleRaw(path, t);

    // A scene may leave every key's target unset and rely on the path tangent,
    // which reads naturally for fly-throughs.
    if (glm::dot(sample.target, sample.target) < 1e-8f) {
        constexpr float kEpsilon = 1e-3f;
        const PathSample ahead = SampleRaw(path, t + kEpsilon);
        const PathSample behind = SampleRaw(path, t - kEpsilon);
        glm::vec3 tangent = ahead.position - behind.position;
        if (glm::dot(tangent, tangent) < 1e-12f) {
            tangent = sample.position - behind.position;
        }
        sample.target = sample.position + tangent;
    }
    return sample;
}

float ApproximatePathLength(const CameraPath& path) {
    if (!path.Valid()) return 0.0f;

    constexpr int kSamples = 256;
    float length = 0.0f;
    glm::vec3 previous = SamplePath(path, 0.0f).position;
    for (int i = 1; i <= kSamples; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kSamples);
        const glm::vec3 current = SamplePath(path, t).position;
        length += glm::length(current - previous);
        previous = current;
    }
    return length;
}

} // namespace Examples::Explorer
