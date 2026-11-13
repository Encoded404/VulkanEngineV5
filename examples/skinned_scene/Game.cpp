module;

#include <SDL3/SDL_keycode.h>
#include <logging/logging_macros.hpp>

module Examples.SkinnedScene.Game;

import std;

import logiface;

import VulkanEngine.GameEngine;
import VulkanEngine.ECS.ComponentRegistry;
import VulkanEngine.ECS.Entity;
import VulkanEngine.GpuResources.MeshData;
import VulkanEngine.Mesh.MeshTypes;
import VulkanEngine.Mesh.SkeletalAnimation;
import VulkanEngine.Components.SkeletonPose;

namespace Examples::SkinnedScene {

namespace {

// Hamilton product of two xyzw quaternions: `a * b` applies `b` first, in `a`'s
// local frame.
[[nodiscard]] std::array<float, 4> MultiplyQuat(const std::array<float, 4>& a,
                                                const std::array<float, 4>& b) {
    const float ax = a[0], ay = a[1], az = a[2], aw = a[3];
    const float bx = b[0], by = b[1], bz = b[2], bw = b[3];
    return {
        aw * bx + ax * bw + ay * bz - az * by,
        aw * by - ax * bz + ay * bw + az * bx,
        aw * bz + ax * by - ay * bx + az * bw,
        aw * bw - ax * bx - ay * by - az * bz,
    };
}

[[nodiscard]] std::array<float, 4> RotationAboutX(const float angle) {
    const float half = 0.5f * angle;
    return {std::sin(half), 0.0f, 0.0f, std::cos(half)};
}

void SetRotation(VulkanEngine::SubMeshTransform& transform,
                 const std::array<float, 4>& q) {
    transform.rotation[0] = q[0];
    transform.rotation[1] = q[1];
    transform.rotation[2] = q[2];
    transform.rotation[3] = q[3];
}

[[nodiscard]] std::array<float, 4> GetRotation(const VulkanEngine::SubMeshTransform& transform) {
    return {transform.rotation[0], transform.rotation[1],
            transform.rotation[2], transform.rotation[3]};
}

} // namespace

Game::Game(const std::filesystem::path& executable_path)
    : exe_dir_(executable_path.parent_path()) {
    setup_token_ = hooks_.on_setup.Register([this](VulkanEngine::Application::ApplicationContext& ctx) -> bool {
        return OnSetup(ctx);
    });
    fixed_update_token_ = hooks_.on_fixed_update.Register([this](VulkanEngine::Application::ApplicationContext& ctx) {
        OnFixedUpdate(ctx);
    });
    frame_update_token_ = hooks_.on_frame_update.Register([this](VulkanEngine::Application::ApplicationContext& ctx) {
        OnFrameUpdate(ctx);
    });
    frame_render_token_ = hooks_.on_frame_render.Register([this](VulkanEngine::Application::ApplicationContext& ctx) {
        OnFrameRender(ctx);
    });
    shutdown_token_ = hooks_.on_shutdown.Register([this](VulkanEngine::Application::ApplicationContext& ctx) {
        OnShutdown(ctx);
    });
}

Game::~Game() = default;

bool Game::OnSetup(VulkanEngine::Application::ApplicationContext& ctx) {
    VulkanEngine::GameConfig config{};
    config.renderer_config.clear_color = {0.08f, 0.09f, 0.11f, 1.0f};
    config.shader_data_dir = (exe_dir_ / "shaders").string();

    if (!engine_game_.Setup(ctx, config)) {
        return false;
    }

    auto& shader_mgr = engine_game_.GetContext().GetShaderManager();
    const auto vert_id = engine_game_.GetContext().GetShaderIds().main_indir_vert;
    const auto frag_id = engine_game_.GetContext().GetShaderIds().standard_mesh_frag;
    if (!engine_game_.InitRenderer(
            ctx, VulkanEngine::ShaderSystem::RendererShaders{
                     .vert = vert_id, .frag = frag_id, .manager = &shader_mgr})) {
        return false;
    }

    // The example's only asset: a two-bone cuboid with JOINTS_0/WEIGHTS_0. It
    // carries a skeleton, one skin and no animation clip.
    const std::filesystem::path model_path = exe_dir_ / "models" / "2BoneRectangle.glb";
    // Bind the glTF material to the engine's fallback material (id 0), which is
    // the checkerboard registered during renderer initialization.
    const std::vector<VulkanEngine::SceneLoader::MaterialId> material_bindings{
        VulkanEngine::SceneLoader::MaterialId{0}};
    VulkanEngine::GpuResources::MeshData mesh_data =
        VulkanEngine::SceneLoader::LoadMeshData(model_path, &material_bindings);
    if (mesh_data.influence_sets == 0 || mesh_data.skeletons.empty() ||
        mesh_data.skins.empty() || mesh_data.bone_weights.size() != mesh_data.vertices.size()) {
        LOGIFACE_LOG(error, "SkinnedScene: " + model_path.string() +
                                " did not import as a skinned mesh; nothing to draw");
        return false;
    }

    // The pose and (later) any animation player own a self-contained copy of the
    // asset, not a pointer into registry storage.
    const auto asset = VulkanEngine::MakeSkeletalAsset(mesh_data);
    const std::uint32_t mesh_id = engine_game_.GetMeshRegistry().Register(mesh_data);

    auto& registry = engine_game_.GetContext().GetComponentRegistry();

    engine_game_.CreateCamera(registry);
    if (auto* camera = engine_game_.GetCamera()) {
        // Look at the cuboid from its long (+X) side so the Z bend is visible.
        camera->position = {7.5f, 2.5f, 2.0f};
        camera->target = {0.0f, 0.0f, 0.0f};
        camera->fov_degrees = 45.0f;
    }

    auto& entity = registry.CreateEntity();
    registry.AddComponent<VulkanEngine::Components::Transform>(entity);
    registry.AddComponent<VulkanEngine::Components::MeshReference>(entity).loaded_mesh_id = mesh_id;

    auto& pose = registry.AddComponent<VulkanEngine::Components::SkeletonPose>(entity);
    pose.asset = asset;
    pose.skeleton_index = 0;
    pose.Rebuild();
    if (pose.local.size() != 2U) {
        LOGIFACE_LOG(error, "SkinnedScene: expected the asset's two-bone skeleton, found " +
                                std::to_string(pose.local.size()) + " joints");
        return false;
    }
    bind_rotations_[0] = GetRotation(pose.local[0]);
    bind_rotations_[1] = GetRotation(pose.local[1]);
    pose_entity_ = entity.GetId();

    engine_game_.MarkSceneValid();

    ctx.quit_action_handle = ctx.input_system->BindAction(
        "quit", VulkanEngine::Input::InputBinding::Key(SDLK_ESCAPE));
    return true;
}

void Game::AdvancePose(const float delta_time) {
    time_ += delta_time;

    auto& registry = engine_game_.GetContext().GetComponentRegistry();
    VulkanEngine::Entity* const entity = registry.TryGetEntity(pose_entity_);
    if (entity == nullptr) {
        return;
    }
    auto* const pose = entity->GetComponent<VulkanEngine::Components::SkeletonPose>();
    if (pose == nullptr || pose->local.size() != 2U) {
        return;
    }

    // Both bones swing about their local X, the root with the lower amplitude and
    // the child counter-rotating so the lower half folds back. The bind rotation
    // is composed on the left, so the delta is applied in the joint's own frame
    // and the motion never accumulates.
    const float phase = time_ * 1.6f;
    const float root_angle = 0.30f * std::sin(phase);
    const float child_angle = -0.65f * std::sin(phase);
    SetRotation(pose->local[0], MultiplyQuat(bind_rotations_[0], RotationAboutX(root_angle)));
    SetRotation(pose->local[1], MultiplyQuat(bind_rotations_[1], RotationAboutX(child_angle)));
    pose->RebuildFromLocal();
}

void Game::OnFixedUpdate(const VulkanEngine::Application::ApplicationContext& ctx) {
    engine_game_.FixedUpdate(ctx);
}

void Game::OnFrameUpdate(const VulkanEngine::Application::ApplicationContext& ctx) {
    // Pose before FrameUpdate: the Bridge phase runs AnimationUpdate (which leaves
    // a settled, player-less pose alone) and then gather, which copies the palette.
    AdvancePose(ctx.frame.delta_time);
    engine_game_.FrameUpdate(ctx);
}

void Game::OnFrameRender(const VulkanEngine::Application::ApplicationContext& ctx) {
    engine_game_.FrameRender(ctx);
}

void Game::OnShutdown(VulkanEngine::Application::ApplicationContext& /*ctx*/) {
    engine_game_.Shutdown();
}

} // namespace Examples::SkinnedScene
