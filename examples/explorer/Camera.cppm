module;

#include <glm/glm.hpp>

// The shared explorer camera: three modes on one state machine.
//
//   Freefly   - first-person fly with drag-to-look and WASD/QE movement.
//   Showcase  - orbit around the scene's focus point, drag-to-rotate and
//               wheel-to-zoom; this is the "model in the middle" mode.
//   Path      - follows one of the scene's declared camera paths (0..N).
//
// Switching modes preserves where the camera is looking from, so the user never
// gets a jump cut; every mode writes the same engine Camera component.
export module Examples.Explorer.Camera;

import std;

import VulkanEngine.Components.Camera;

import Examples.Explorer.Math;
import Examples.Explorer.Options;

export namespace Examples::Explorer {

enum class CameraMode : std::uint8_t {
    Freefly = 0,
    Showcase = 1,
    Path = 2,
};

[[nodiscard]] const char* ToString(CameraMode mode) noexcept;

// One frame of already-filtered view input. The explorer gathers it from the
// input system (and suppresses it while ImGui owns the mouse), which keeps this
// class free of SDL and ImGui.
struct CameraInput {
    bool look_drag = false;   // freefly: right mouse held
    bool orbit_drag = false;  // showcase: left mouse held
    bool pan_drag = false;    // showcase: middle mouse held
    glm::vec2 mouse_delta{0.0f, 0.0f};
    float wheel = 0.0f;
    glm::vec2 move{0.0f, 0.0f};  // x = strafe right, y = forward
    float lift = 0.0f;           // -1 = down, +1 = up
    bool boost = false;
};

class ExplorerCamera {
public:
    ExplorerCamera();

    // ── Mode ────────────────────────────────────────────────────────────
    void SetMode(CameraMode mode);
    [[nodiscard]] CameraMode Mode() const noexcept { return mode_; }

    // ── Scene framing ───────────────────────────────────────────────────
    void SetBounds(const Bounds& bounds);
    [[nodiscard]] const Bounds& GetBounds() const noexcept { return bounds_; }
    // Places the camera so the whole scene is visible, and resets orbit angles.
    void FrameBounds();
    // Alias used by the "reset view" button.
    void ResetView() { FrameBounds(); }

    // ── Paths ───────────────────────────────────────────────────────────
    void SetPaths(std::span<const CameraPath> paths);
    [[nodiscard]] const std::vector<CameraPath>& Paths() const noexcept { return paths_; }
    [[nodiscard]] bool HasPaths() const noexcept { return !paths_.empty(); }
    [[nodiscard]] int PathIndex() const noexcept { return path_index_; }
    void SelectPath(int index);
    [[nodiscard]] bool PathPlaying() const noexcept { return path_playing_; }
    void SetPathPlaying(bool playing) { path_playing_ = playing; }
    [[nodiscard]] float PathPosition() const noexcept { return path_t_; }
    void SetPathPosition(float t);
    [[nodiscard]] float PathSpeed() const noexcept { return path_speed_; }
    void SetPathSpeed(float units_per_second);
    [[nodiscard]] bool PathLoops() const noexcept { return loop_path_; }
    void SetPathLoops(bool loop) { loop_path_ = loop; }

    // ── Per frame ───────────────────────────────────────────────────────
    void Update(const CameraInput& input, float delta_time);
    void ApplyTo(VulkanEngine::Components::Camera& camera) const;

    // Declares the camera's own controls on an OptionsUi (built once).
    void BuildOptions(OptionsUi& ui);

private:
    struct Pose {
        glm::vec3 position{0.0f, 0.0f, 0.0f};
        glm::vec3 forward{0.0f, 0.0f, -1.0f};
    };

    [[nodiscard]] Pose CurrentPose() const;
    [[nodiscard]] PathSample CurrentPathSample() const;
    [[nodiscard]] float RadiusOr(float fallback) const noexcept {
        return bounds_.valid ? bounds_.Radius() : fallback;
    }
    void SyncStateForMode(CameraMode next);

    CameraMode mode_ = CameraMode::Showcase;
    Bounds bounds_{};

    // Freefly
    glm::vec3 freefly_position_{0.0f, 2.0f, 6.0f};
    float freefly_yaw_degrees_ = 0.0f;
    float freefly_pitch_degrees_ = -10.0f;
    float move_speed_ = 5.0f;
    float look_sensitivity_ = 0.18f;
    bool invert_look_y_ = false;

    // Showcase
    glm::vec3 orbit_target_{0.0f, 0.0f, 0.0f};
    float orbit_distance_ = 6.0f;
    float orbit_distance_target_ = 6.0f;
    float orbit_yaw_degrees_ = 25.0f;
    float orbit_pitch_degrees_ = 18.0f;
    float orbit_pan_speed_ = 1.0f;
    float auto_rotate_degrees_per_second_ = 0.0f;

    // Shared lens
    float fov_degrees_ = 60.0f;
    float near_plane_ = 0.05f;
    float far_plane_ = 2000.0f;
    bool auto_clip_planes_ = true;

    // Path
    std::vector<CameraPath> paths_{};
    std::vector<float> path_lengths_{};
    int path_index_ = -1;
    float path_t_ = 0.0f;
    bool path_playing_ = false;
    bool loop_path_ = true;
    float path_speed_ = 4.0f;  // world units per second
};

} // namespace Examples::Explorer
