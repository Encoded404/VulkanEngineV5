module;

export module Examples.SkinnedScene.Game;

import std;

export import VulkanEngine.GameEngine;
import VulkanShared.CallbackList;
import VulkanEngine.ECS.Entity;

export namespace Examples::SkinnedScene {

// Minimal skeletal-animation example.
//
// One entity draws the two-bone GLB in `models/2BoneRectangle.glb`. The asset has
// no animation clips, so the example drives `SkeletonPose` by hand: every frame
// it restores the bind-pose local rotations, applies a sine-wave rotation to both
// joints, and rebuilds the palette before the engine's Bridge phase runs gather.
// The builtin skinning modifier then deforms the mesh in the deform pass and the
// standard mesh shaders draw it.
class Game {
public:
    explicit Game(const std::filesystem::path& executable_path);
    ~Game();

    Game(const Game&) = delete;
    Game& operator=(const Game&) = delete;

    [[nodiscard]] const VulkanEngine::Application::ApplicationHooks& GetHooks() const { return hooks_; }

private:
    bool OnSetup(VulkanEngine::Application::ApplicationContext& ctx);
    void OnFixedUpdate(const VulkanEngine::Application::ApplicationContext& ctx);
    void OnFrameUpdate(const VulkanEngine::Application::ApplicationContext& ctx);
    void OnFrameRender(const VulkanEngine::Application::ApplicationContext& ctx);
    void OnShutdown(VulkanEngine::Application::ApplicationContext& ctx);

    // Advances the sine wave and rewrites the pose's two local rotations.
    void AdvancePose(float delta_time);

    VulkanEngine::Application::ApplicationHooks hooks_{};

    VulkanShared::ScopedHandle<bool(VulkanEngine::Application::ApplicationContext&)> setup_token_{};
    VulkanShared::ScopedHandle<void(VulkanEngine::Application::ApplicationContext&)> fixed_update_token_{};
    VulkanShared::ScopedHandle<void(VulkanEngine::Application::ApplicationContext&)> frame_update_token_{};
    VulkanShared::ScopedHandle<void(VulkanEngine::Application::ApplicationContext&)> frame_render_token_{};
    VulkanShared::ScopedHandle<void(VulkanEngine::Application::ApplicationContext&)> shutdown_token_{};

    std::filesystem::path exe_dir_{};
    // Local rotations of the two joints in the asset's bind pose, xyzw. Captured
    // once after the first Rebuild(); the animation composes a rotation about X
    // on top of each so it never accumulates drift.
    std::array<std::array<float, 4>, 2> bind_rotations_{};
    // Re-fetch the component through this handle each frame; component storage can
    // move on a structural change, so no component pointer is cached.
    VulkanEngine::EntityId pose_entity_{};
    float time_ = 0.0f;
    // Direct-mode: device core + default renderer only.
    VulkanEngine::GameEngine engine_game_{VulkanEngine::EngineFeatures{}};
};

} // namespace Examples::SkinnedScene
