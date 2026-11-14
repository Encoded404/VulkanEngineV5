module;

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

module Examples.Explorer.Camera;

import std;

import VulkanEngine.Components.Camera;

import Examples.Explorer.Math;
import Examples.Explorer.Options;

namespace Examples::Explorer {

namespace {

constexpr float kPitchLimit = 85.0f;

[[nodiscard]] float NormalizeAngle(float degrees) noexcept {
    degrees = std::fmod(degrees + 180.0f, 360.0f);
    if (degrees < 0.0f) degrees += 360.0f;
    return degrees - 180.0f;
}

} // namespace

const char* ToString(CameraMode mode) noexcept {
    switch (mode) {
        case CameraMode::Freefly: return "Freefly";
        case CameraMode::Showcase: return "Showcase";
        case CameraMode::Path: return "Path";
    }
    return "Showcase";
}

ExplorerCamera::ExplorerCamera() = default;

void ExplorerCamera::SetBounds(const Bounds& bounds) {
    bounds_ = bounds;
}

void ExplorerCamera::FrameBounds() {
    const glm::vec3 center = bounds_.Center();
    const float radius = RadiusOr(1.0f);

    orbit_target_ = center;
    orbit_yaw_degrees_ = 25.0f;
    orbit_pitch_degrees_ = 18.0f;
    orbit_distance_target_ = radius * 2.6f;
    orbit_distance_ = orbit_distance_target_;

    // Park the freefly camera on the -Z side looking at the scene, which is the
    // same three-quarter view Showcase starts from.
    freefly_yaw_degrees_ = 0.0f;
    freefly_pitch_degrees_ = -8.0f;
    freefly_position_ = center - DirectionFromAngles(freefly_yaw_degrees_, freefly_pitch_degrees_) *
                                     (radius * 2.6f);

    path_t_ = 0.0f;
}

void ExplorerCamera::SetPaths(std::span<const CameraPath> paths) {
    paths_.clear();
    for (const CameraPath& path : paths) {
        // A path with fewer than two keys has no geometry to sample; keeping it
        // would enable Path mode on a scene that cannot move the camera.
        if (path.Valid()) paths_.push_back(path);
    }
    path_lengths_.clear();
    path_lengths_.reserve(paths_.size());
    for (const CameraPath& path : paths_) {
        path_lengths_.push_back(ApproximatePathLength(path));
    }
    path_playing_ = false;

    if (paths_.empty()) {
        path_index_ = -1;
        path_t_ = 0.0f;
        path_playing_ = false;
        if (mode_ == CameraMode::Path) {
            mode_ = CameraMode::Showcase;
        }
        return;
    }

    if (path_index_ < 0 || path_index_ >= static_cast<int>(paths_.size())) {
        path_index_ = 0;
    }
    path_t_ = 0.0f;
}

void ExplorerCamera::SelectPath(int index) {
    if (paths_.empty()) {
        path_index_ = -1;
        return;
    }
    path_index_ = std::clamp(index, 0, static_cast<int>(paths_.size()) - 1);
    path_t_ = 0.0f;
}

void ExplorerCamera::SetPathPosition(float t) {
    path_t_ = loop_path_ ? (t - std::floor(t)) : std::clamp(t, 0.0f, 1.0f);
}

void ExplorerCamera::SetPathSpeed(float units_per_second) {
    path_speed_ = std::clamp(units_per_second, 0.05f, 200.0f);
}

PathSample ExplorerCamera::CurrentPathSample() const {
    if (path_index_ < 0 || path_index_ >= static_cast<int>(paths_.size())) return {};
    return SamplePath(paths_[static_cast<std::size_t>(path_index_)], path_t_);
}

ExplorerCamera::Pose ExplorerCamera::CurrentPose() const {
    Pose pose{};
    switch (mode_) {
        case CameraMode::Freefly: {
            pose.position = freefly_position_;
            pose.forward = DirectionFromAngles(freefly_yaw_degrees_, freefly_pitch_degrees_);
            break;
        }
        case CameraMode::Showcase: {
            const glm::vec3 offset =
                OrbitOffsetFromAngles(orbit_yaw_degrees_, orbit_pitch_degrees_) *
                std::max(orbit_distance_, 1e-3f);
            pose.position = orbit_target_ + offset;
            pose.forward = -glm::normalize(offset);
            break;
        }
        case CameraMode::Path: {
            const PathSample sample = CurrentPathSample();
            pose.position = sample.position;
            glm::vec3 forward = sample.target - sample.position;
            if (glm::dot(forward, forward) < 1e-10f) {
                forward = DirectionFromAngles(0.0f, 0.0f);
            }
            pose.forward = glm::normalize(forward);
            break;
        }
    }
    return pose;
}

void ExplorerCamera::SyncStateForMode(CameraMode next) {
    // Capture the pose before changing mode, so the new mode starts exactly
    // where the old one was looking from instead of jumping.
    const Pose pose = CurrentPose();
    switch (next) {
        case CameraMode::Freefly: {
            freefly_position_ = pose.position;
            const glm::vec2 angles = AnglesFromDirection(pose.forward);
            freefly_yaw_degrees_ = NormalizeAngle(angles.x);
            freefly_pitch_degrees_ = std::clamp(angles.y, -kPitchLimit, kPitchLimit);
            break;
        }
        case CameraMode::Showcase: {
            const glm::vec3 center = bounds_.valid ? bounds_.Center() : pose.position;
            glm::vec3 offset = pose.position - center;
            float distance = glm::length(offset);
            if (distance < 1e-4f) {
                offset = -pose.forward;
                distance = RadiusOr(1.0f);
            }
            orbit_target_ = center;
            orbit_distance_ = distance;
            orbit_distance_target_ = distance;
            const glm::vec2 angles = OrbitAnglesFromOffset(offset);
            orbit_yaw_degrees_ = angles.x;
            orbit_pitch_degrees_ = std::clamp(angles.y, -kPitchLimit, kPitchLimit);
            break;
        }
        case CameraMode::Path: {
            if (path_index_ < 0 && !paths_.empty()) path_index_ = 0;
            path_t_ = 0.0f;
            break;
        }
    }
}

void ExplorerCamera::SetMode(CameraMode mode) {
    if (mode == CameraMode::Path && !HasPaths()) {
        mode = CameraMode::Showcase;
    }
    if (mode == mode_) return;
    SyncStateForMode(mode);
    mode_ = mode;
    if (mode_ == CameraMode::Path && paths_.empty()) {
        mode_ = CameraMode::Showcase;
    }
}

void ExplorerCamera::Update(const CameraInput& input, float delta_time) {
    const float dt = std::clamp(delta_time, 0.0f, 0.1f);
    if (dt <= 0.0f) return;

    switch (mode_) {
        case CameraMode::Freefly: {
            if (input.look_drag) {
                freefly_yaw_degrees_ += input.mouse_delta.x * look_sensitivity_;
                const float pitch_sign = invert_look_y_ ? -1.0f : 1.0f;
                freefly_pitch_degrees_ -= input.mouse_delta.y * look_sensitivity_ * pitch_sign;
                freefly_yaw_degrees_ = NormalizeAngle(freefly_yaw_degrees_);
                freefly_pitch_degrees_ = std::clamp(freefly_pitch_degrees_, -kPitchLimit, kPitchLimit);
            }

            if (std::abs(input.wheel) > 0.0f) {
                // Wheel adjusts travel speed in freefly; clamping keeps it usable
                // on scenes measured in millimetres and in kilometres.
                move_speed_ = std::clamp(move_speed_ * std::exp(input.wheel * 0.15f), 0.05f, 500.0f);
            }

            glm::vec3 forward = DirectionFromAngles(freefly_yaw_degrees_, 0.0f);
            const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3{0.0f, 1.0f, 0.0f}));
            glm::vec3 move = forward * input.move.y + right * input.move.x;
            if (glm::dot(move, move) > 1.0f) move = glm::normalize(move);

            const float speed = move_speed_ * (input.boost ? 4.0f : 1.0f);
            freefly_position_ += move * speed * dt;
            freefly_position_ += glm::vec3{0.0f, input.lift, 0.0f} * speed * dt;
            break;
        }
        case CameraMode::Showcase: {
            if (input.orbit_drag) {
                orbit_yaw_degrees_ -= input.mouse_delta.x * look_sensitivity_;
                // Drag down raises the camera above the target (the Blender /
                // three.js orbit convention). Freefly keeps the FPS convention
                // where mouse-down looks down. "Invert look Y" flips both.
                const float pitch_sign = invert_look_y_ ? -1.0f : 1.0f;
                orbit_pitch_degrees_ += input.mouse_delta.y * look_sensitivity_ * pitch_sign;
                orbit_yaw_degrees_ = NormalizeAngle(orbit_yaw_degrees_);
                orbit_pitch_degrees_ = std::clamp(orbit_pitch_degrees_, -kPitchLimit, kPitchLimit);
            }

            if (input.pan_drag) {
                const glm::vec3 offset =
                    OrbitOffsetFromAngles(orbit_yaw_degrees_, orbit_pitch_degrees_);
                const glm::vec3 forward = -glm::normalize(offset);
                const glm::vec3 right =
                    glm::normalize(glm::cross(forward, glm::vec3{0.0f, 1.0f, 0.0f}));
                const glm::vec3 up = glm::normalize(glm::cross(right, forward));
                const float scale = orbit_distance_ * 0.0018f * orbit_pan_speed_;
                orbit_target_ += (-right * input.mouse_delta.x + up * input.mouse_delta.y) * scale;
            }

            if (std::abs(input.wheel) > 0.0f) {
                orbit_distance_target_ *= std::exp(-input.wheel * 0.15f);
            }

            const float radius = RadiusOr(1.0f);
            orbit_distance_target_ =
                std::clamp(orbit_distance_target_, radius * 0.02f, radius * 40.0f);
            orbit_distance_ = Damp(orbit_distance_, orbit_distance_target_, 12.0f, dt);

            orbit_yaw_degrees_ = NormalizeAngle(
                orbit_yaw_degrees_ + auto_rotate_degrees_per_second_ * dt);
            break;
        }
        case CameraMode::Path: {
            if (!path_playing_ || paths_.empty()) break;
            const auto index = static_cast<std::size_t>(std::max(path_index_, 0));
            if (index >= paths_.size()) break;
            const float length = (index < path_lengths_.size() && path_lengths_[index] > 1e-3f)
                                     ? path_lengths_[index]
                                     : 1.0f;
            path_t_ += (path_speed_ * dt) / length;
            if (path_t_ > 1.0f) {
                if (loop_path_) {
                    path_t_ = std::fmod(path_t_, 1.0f);
                } else {
                    path_t_ = 1.0f;
                    path_playing_ = false;
                }
            }
            break;
        }
    }
}

void ExplorerCamera::ApplyTo(VulkanEngine::Components::Camera& camera) const {
    const Pose pose = CurrentPose();
    camera.position = pose.position;
    camera.forward = pose.forward;
    camera.up = glm::vec3{0.0f, 1.0f, 0.0f};
    camera.orientation = VulkanEngine::Components::CameraOrientation::Direction;
    camera.fov_degrees = fov_degrees_;

    if (auto_clip_planes_) {
        const float radius = RadiusOr(1.0f);
        // The near plane tracks the scene scale, not the camera distance: a
        // 0.05 near plane on a kilometre-sized scene is depth-buffer suicide.
        camera.near_plane = std::max(0.01f, radius * 0.0015f);
        camera.far_plane = std::max(500.0f, radius * 120.0f);
    } else {
        camera.near_plane = near_plane_;
        camera.far_plane = far_plane_;
    }
    camera.projection_mode = VulkanEngine::Components::ProjectionMode::Perspective;
}

void ExplorerCamera::BuildOptions(OptionsUi& ui) {
    ui.Section("Camera");
    ui.SliderFloat("Field of view", fov_degrees_, 20.0f, 110.0f, "%.0f deg");
    ui.SliderFloat("Move speed", move_speed_, 0.1f, 60.0f, "%.2f u/s",
                   "Freefly travel speed. The mouse wheel also changes it.");
    ui.SliderFloat("Look sensitivity", look_sensitivity_, 0.02f, 1.0f, "%.2f");
    ui.Checkbox("Invert look Y", invert_look_y_);
    ui.SliderFloat("Auto-rotate", auto_rotate_degrees_per_second_, -90.0f, 90.0f, "%.0f deg/s",
                   "Showcase mode turntable speed.");
    ui.SliderFloat("Pan speed", orbit_pan_speed_, 0.1f, 4.0f, "%.2f");
    ui.Checkbox("Auto clip planes", auto_clip_planes_,
                "Derive near/far from the scene radius instead of the fixed values.");
    if (!auto_clip_planes_) {
        ui.SliderFloat("Near plane", near_plane_, 0.001f, 10.0f, "%.3f");
        ui.SliderFloat("Far plane", far_plane_, 10.0f, 100000.0f, "%.1f");
    }
    ui.Button("Frame scene", [this] { FrameBounds(); },
              "Shortcut: F. Re-centers and re-fits the current mode.");
}

} // namespace Examples::Explorer
