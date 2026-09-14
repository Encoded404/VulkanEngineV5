module;
#include <glm/glm.hpp> // NOLINT(misc-include-cleaner)

#include <SDL3/SDL_keycode.h>
#include <imgui.h>

#include <logging/logging_macros.hpp>

module Examples.InfiniteRunner.Game;

import std;

import logiface;

import VulkanEngine.GameEngine;
import VulkanEngine.GpuResources.MeshData;
import VulkanEngine.ShaderManager;
import VulkanEngine.Components.MaterialOverride;

namespace Examples::InfiniteRunner::Game {

namespace {

// ── Playfield tuning ──
constexpr float kCorridorHalf = 3.5f;   // half-width of the play corridor
constexpr float kWallHeight = 2.25f;
constexpr float kWallDepth = 0.6f;
constexpr float kPlayerSize = 0.9f;
constexpr float kWallSpawnZ = -150.0f;   // far end (camera looks toward -Z)
constexpr float kWallInitialSpawnZ = -10.0f;
constexpr float kWallRecycleZ = 8.0f;   // once a wall passes this, wrap it back
constexpr float kWallSpacingPowScaling = 0.9f;
constexpr int kWallCount = 8;
constexpr std::pair<float, float> kWallHoleSizes = {0.85f, 1.1f};
constexpr float kWallHoleSizePowScaling = 0.15f;
constexpr float kWallSpeed = 15.0f;     // units/second toward the player
constexpr float kPlayerSpeed = 9.0f;    // units/second sideways

constexpr float kDifficultyScalingDivider = 100.0f;

constexpr float WallSpacing() {
    return (kWallRecycleZ - kWallSpawnZ) / static_cast<float>(kWallCount);
}

// Builds a unit cube (centred on the origin, half-extent 0.5) as LoadedMeshData.
// Bounding volumes and the indexed-draw vertex window are filled in later by
// MeshRegistry::Register via EnsureSubmeshBounds.
VulkanEngine::SceneLoader::LoadedMeshData MakeUnitCube() {
    VulkanEngine::SceneLoader::LoadedMeshData cube{};

    const auto add_face = [&cube](const glm::vec3& normal, const glm::vec3& up) {
        const glm::vec3 right = glm::normalize(glm::cross(up, normal));
        const glm::vec3 center = normal * 0.5f;
        const glm::vec3 corners[4] = {
            center - right * 0.5f - up * 0.5f,
            center + right * 0.5f - up * 0.5f,
            center + right * 0.5f + up * 0.5f,
            center - right * 0.5f + up * 0.5f,
        };
        constexpr float uvs[4][2] = {{0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f}, {0.0f, 0.0f}};

        const auto base = static_cast<std::uint32_t>(cube.positions.size() / 3U);
        for (int i = 0; i < 4; ++i) {
            cube.positions.push_back(corners[i].x);
            cube.positions.push_back(corners[i].y);
            cube.positions.push_back(corners[i].z);
            cube.normals.push_back(normal.x);
            cube.normals.push_back(normal.y);
            cube.normals.push_back(normal.z);
            cube.uvs.push_back(uvs[i][0]);
            cube.uvs.push_back(uvs[i][1]);
        }
        cube.indices.insert(cube.indices.end(),
                            {base + 0U, base + 1U, base + 2U, base + 2U, base + 3U, base + 0U});
    };

    add_face({0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f});
    add_face({0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f});
    add_face({1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
    add_face({-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
    add_face({0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
    add_face({0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, -1.0f});

    VulkanEngine::SubMesh submesh{};
    submesh.index_start = 0;
    submesh.index_count = static_cast<std::uint32_t>(cube.indices.size());
    cube.submeshes.push_back(submesh);
    return cube;
}

struct SunSettings {
    float azimuth_degrees   = 45.0f;   // rotation about +Y
    float elevation_degrees = 45.0f;   // above horizon (default 0.5,-0.707,0.5 ≈ 45/45)
    float strength          = 2.0f;    // direct multiplier  -> Light::color.w
    glm::vec3 color         = {1.0f, 0.95f, 0.9f};
    float ambient_strength  = 1.0f;    // -> SceneHeader::ambient_color.a
    glm::vec3 ambient_tint  = {0.03f, 0.03f, 0.03f};
};

// Direction the light travels, from an azimuth/elevation pair.
glm::vec3 SunTravelDirection(float azimuth_deg, float elevation_deg) {
    const float az = glm::radians(azimuth_deg);
    const float el = glm::radians(elevation_deg);
    return glm::normalize(glm::vec3{
        std::cos(el) * std::sin(az),
        -std::sin(el),
        std::cos(el) * std::cos(az),
    });
}

void ApplySun(VulkanEngine::GameEngine& engine, const SunSettings& sun) {
    using VulkanEngine::SceneRenderer::SceneHeader;
    using VulkanEngine::SceneRenderer::Light;

    const glm::vec3 dir = SunTravelDirection(sun.azimuth_degrees, sun.elevation_degrees);

    SceneHeader header{};
    header.ambient_color[0] = sun.ambient_tint.r;
    header.ambient_color[1] = sun.ambient_tint.g;
    header.ambient_color[2] = sun.ambient_tint.b;
    header.ambient_color[3] = sun.ambient_strength;
    header.sun_direction[0] = dir.x;
    header.sun_direction[1] = dir.y;
    header.sun_direction[2] = dir.z;
    header.sun_color[0] = sun.color.r;
    header.sun_color[1] = sun.color.g;
    header.sun_color[2] = sun.color.b;
    header.sun_color[3] = sun.strength;

    Light sun_light{};
    sun_light.direction[0] = dir.x;
    sun_light.direction[1] = dir.y;
    sun_light.direction[2] = dir.z;
    sun_light.color[0] = sun.color.r;
    sun_light.color[1] = sun.color.g;
    sun_light.color[2] = sun.color.b;
    sun_light.color[3] = sun.strength;   // intensity
    sun_light.position[3] = 0.0f;        // 0 = directional

    std::array<Light, 1> lights = {sun_light};
    header.light_count = 1;

    engine.GetSceneRenderer().UploadLighting(header, lights, engine.GetStagingManager());
}

} // namespace

Game::Game(const std::filesystem::path& executable_path)
    : exe_dir_(executable_path.parent_path()) {
    setup_token_ = hooks_.on_setup.Register([this](VulkanEngine::Application::ApplicationContext& ctx) -> bool {
        return OnSetup(ctx);
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

VulkanEngine::Components::Transform* Game::CreateCubeEntity(
    const float x, const float y, const float z,
    const float sx, const float sy, const float sz,
    const VulkanEngine::MaterialManager::MaterialRef material) {
    auto& registry = engine_game_.GetContext().GetComponentRegistry();

    auto& entity = registry.CreateEntity();
    auto& transform = registry.AddComponent<VulkanEngine::Components::Transform>(entity);
    // Run Initialize() now so the per-frame component update does not reset the
    // transform we are about to write on the first frame.
    transform.DispatchUpdate(0.0f);
    transform.position = glm::vec3{x, y, z};
    transform.scale = glm::vec3{sx, sy, sz};

    auto& mesh_ref = registry.AddComponent<VulkanEngine::Components::MeshReference>(entity);
    mesh_ref.loaded_mesh_id = cube_mesh_id_;

    // One cube mesh for everything; the colour comes from the per-object
    // material override. The override adopts this mesh on first resolve.
    auto& override_comp = registry.AddComponent<VulkanEngine::Components::MaterialOverride>(entity);
    override_comp.Set(0, material);

    return &transform;
}

bool Game::OnSetup(VulkanEngine::Application::ApplicationContext& ctx) {
    // 1. Engine setup + renderer using the engine's standard PBR mesh shader.
    VulkanEngine::GameConfig config{};
    config.enable_imgui = true;
    config.renderer_config.clear_color = {0.8f, 0.8f, 0.80f, 1.0f};
    config.shader_data_dir = (exe_dir_ / "shaders").string();

    if (!engine_game_.Setup(ctx, config)) {
        return false;
    }

    auto& shader_mgr = engine_game_.GetContext().GetShaderManager();
    auto& shader_ids = engine_game_.GetContext().GetShaderIds();
    if (!engine_game_.InitRenderer(ctx, shader_ids.main_indir_vert,
                                   shader_ids.standard_mesh_frag, &shader_mgr)) {
        return false;
    }

    SunSettings sun{};
    sun.azimuth_degrees   = 180.0f;  // light from the left
    sun.elevation_degrees = 40.0f;   // low afternoon sun
    sun.strength          = 1.5f;
    ApplySun(engine_game_, sun);

    // 2. Solid-colour materials uploaded to the bindless heap.
    auto& material_mgr = engine_game_.GetContext().GetMaterialManager();
    auto& resource_mgr = engine_game_.GetContext().GetResourceManager();

    const auto make_material = [&](const std::array<std::uint8_t, 4>& rgba,
                                   const float roughness) {
        auto texture = VulkanEngine::DefaultTextureFactory::CreateSolidColorTexture(resource_mgr, rgba);
        const std::uint32_t slot = engine_game_.UploadTextureToBindless(ctx, texture.get());
        textures_.push_back(std::move(texture));

        auto handle = material_mgr.Register<VulkanEngine::TechniqueManager::DefaultMeshTechnique>(
            VulkanEngine::MaterialManager::BlendMode::Opaque,
            VulkanEngine::TechniqueManager::DefaultMeshPerMaterialData{
                .albedo_texture = slot,
                .roughness_factor = roughness,
                .metallic_factor = 0.0f,
                .ao_factor = 1.0f,
            });
        return handle.Ref();
    };

    player_material_ = make_material({70, 200, 255, 255}, 0.45f);
    wall_material_ = make_material({225, 90, 70, 255}, 0.85f);
    floor_material_ = make_material({40, 45, 58, 255}, 0.95f);

    // 3. One procedural cube registered once, reused by every entity.
    const auto cube = VulkanEngine::SceneLoader::ToMeshData(MakeUnitCube());
    cube_mesh_id_ = engine_game_.GetMeshRegistry().Register(cube);

    engine_game_.MarkSceneValid();

    // 4. Camera: fixed behind the player, looking down the corridor (-Z).
    // Orientation comes from `forward` rather than a look-at `target`, so the
    // camera stays parallel to the corridor instead of yawing toward a fixed
    // world point as the player moves sideways.
    auto& registry = engine_game_.GetContext().GetComponentRegistry();
    camera_ = &engine_game_.CreateCamera(registry);
    camera_->position = glm::vec3{0.0f, 2.0f, 6.5f};
    camera_->up = glm::vec3{0.0f, 1.0f, 0.0f};
    camera_->orientation = VulkanEngine::Components::CameraOrientation::Direction;
    camera_->forward = glm::normalize(glm::vec3{0.0f, -1.6f, -12.5f});
    camera_->fov_degrees = 60.0f;
    camera_->near_plane = 0.1f;
    camera_->far_plane = 220.0f;

    // 5. Floor + player + wall pool.
    float floor_height = 0.2f;
    CreateCubeEntity(0.0f, -(kPlayerSize / 2) - (floor_height / 2), -20.0f, 2.0f * kCorridorHalf + 2.0f, floor_height, 120.0f, floor_material_);
    player_transform_ = CreateCubeEntity(0.0f, 0.0f, 0.0f, kPlayerSize, kPlayerSize, kPlayerSize, player_material_);

    walls_.reserve(kWallCount);
    for (int i = 0; i < kWallCount; ++i) {
        Wall wall{};
        wall.left = CreateCubeEntity(0.0f, 0.0f, 0.0f, 1.0f, kWallHeight, kWallDepth, wall_material_);
        wall.right = CreateCubeEntity(0.0f, 0.0f, 0.0f, 1.0f, kWallHeight, kWallDepth, wall_material_);
        wall.z = kWallInitialSpawnZ + kWallSpawnZ + static_cast<float>(i) * WallSpacing();
        RandomizeWall(wall);
        ApplyWallTransform(wall);
        walls_.push_back(wall);
    }

    // 6. Input.
    auto* input = ctx.input_system;
    move_left_a_ = input->BindAction("move_left", VulkanEngine::Input::InputBinding::Key(SDLK_A));
    move_left_arrow_ = input->BindAction("move_left_arrow", VulkanEngine::Input::InputBinding::Key(SDLK_LEFT));
    move_right_d_ = input->BindAction("move_right", VulkanEngine::Input::InputBinding::Key(SDLK_D));
    move_right_arrow_ = input->BindAction("move_right_arrow", VulkanEngine::Input::InputBinding::Key(SDLK_RIGHT));
    restart_handle_ = input->BindAction("restart", VulkanEngine::Input::InputBinding::Key(SDLK_R));
    ctx.quit_action_handle = input->BindAction("quit", VulkanEngine::Input::InputBinding::Key(SDLK_ESCAPE));

    // 7. HUD.
    if (auto* imgui = engine_game_.GetImGuiSystem()) {
        imgui_draw_handle_ = imgui->draw_callbacks.Register([this]() {
            ImGui::SetNextWindowPos(ImVec2(12.0f, 12.0f), ImGuiCond_Always);
            ImGui::Begin("Infinite Runner", nullptr,
                         ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove);
            ImGui::Text("Score: %d", score_);
            ImGui::Separator();
            if (game_over_) {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "GAME OVER");
                ImGui::TextUnformatted("Press R to restart");
            } else {
                ImGui::TextUnformatted("A/D or Left/Right to move");
                ImGui::TextUnformatted("R to restart, Esc to quit");
            }
            ImGui::End();
        });
    }

    return true;
}

void Game::RandomizeWall(Wall& wall) {
    float difficulty = 2 - std::pow(std::max(1, score_), kWallHoleSizePowScaling);
    std::uniform_real_distribution<float> half_dist(kWallHoleSizes.first * difficulty, kWallHoleSizes.second * difficulty);
    const float gap_half = half_dist(rng_);

    constexpr float margin = 0.25f;
    const float limit = kCorridorHalf - gap_half - margin;
    std::uniform_real_distribution<float> center_dist(-limit, limit);
    const float gap_center = center_dist(rng_);

    wall.gap_left_x = gap_center - gap_half;
    wall.gap_right_x = gap_center + gap_half;
    wall.passed = false;
}

void Game::ApplyWallTransform(Wall& wall) {
    const float left_width = wall.gap_left_x + kCorridorHalf;
    const float right_width = kCorridorHalf - wall.gap_right_x;

    wall.left->position = glm::vec3{(-kCorridorHalf + wall.gap_left_x) * 0.5f, 0.0f, wall.z};
    wall.left->scale = glm::vec3{std::max(left_width, 0.05f), kWallHeight, kWallDepth};

    wall.right->position = glm::vec3{(wall.gap_right_x + kCorridorHalf) * 0.5f, 0.0f, wall.z};
    wall.right->scale = glm::vec3{std::max(right_width, 0.05f), kWallHeight, kWallDepth};
}

void Game::ResetWalls() {
    float difficulty = std::pow(std::max(1, score_), kWallSpacingPowScaling);
    for (int i = 0; i < static_cast<int>(walls_.size()); ++i) {
        auto& wall = walls_[static_cast<std::size_t>(i)];
        wall.z = kWallInitialSpawnZ + kWallSpawnZ + static_cast<float>(i) * WallSpacing() * std::max(1.0f, difficulty / kDifficultyScalingDivider);
        RandomizeWall(wall);
        ApplyWallTransform(wall);
    }
}

void Game::ResetRun() {
    player_x_ = 0.0f;
    score_ = 0;
    game_over_ = false;
    player_transform_->position = glm::vec3{0.0f, 0.0f, 0.0f};
    ResetWalls();
}

void Game::UpdatePlayer(const VulkanEngine::Application::ApplicationContext& ctx, const float delta_time) {


    float direction = 0.0f;
    auto* input = ctx.input_system;
    if (input->IsActionActive(move_left_a_) || input->IsActionActive(move_left_arrow_)) {
        direction -= 1.0f;
    }
    if (input->IsActionActive(move_right_d_) || input->IsActionActive(move_right_arrow_)) {
        direction += 1.0f;
    }

    const float limit = kCorridorHalf - kPlayerSize * 0.5f;
    player_x_ = std::clamp(player_x_ + direction * kPlayerSpeed * delta_time, -limit, limit);
    player_transform_->position = glm::vec3{player_x_, 0.0f, 0.0f};
    camera_->position.x = player_x_;
}

void Game::UpdateWalls(const float delta_time) {
    float difficulty = std::max(1, score_);

    for (auto& wall : walls_) {
        wall.z += kWallSpeed * delta_time  * std::max(1.0f, difficulty / kDifficultyScalingDivider);

        if (wall.z > kWallRecycleZ) {
            wall.z -= (kWallRecycleZ - kWallSpawnZ);
            RandomizeWall(wall);
        }

        if (!wall.passed && wall.z >= 0.0f) {
            wall.passed = true;
            ++score_;
        }

        ApplyWallTransform(wall);
    }
}

bool Game::PlayerHitsAnyWall() const {
    const float half = kPlayerSize * 0.5f;
    const float player_min_x = player_x_ - half;
    const float player_max_x = player_x_ + half;
    const float z_reach = kWallDepth * 0.5f + half;

    for (const auto& wall : walls_) {
        if (std::fabs(wall.z) > z_reach) {
            continue;
        }
        if (player_min_x < wall.gap_left_x && player_max_x > -kCorridorHalf) {
            return true; // left block
        }
        if (player_max_x > wall.gap_right_x && player_min_x < kCorridorHalf) {
            return true; // right block
        }
    }
    return false;
}

void Game::OnFrameUpdate(const VulkanEngine::Application::ApplicationContext& ctx) {
    if (ctx.input_system->WasActionStarted(restart_handle_)) {
        ResetRun();
    }

    const float delta_time = ctx.frame.delta_time;
    if (!game_over_) {
        UpdatePlayer(ctx, delta_time);
        UpdateWalls(delta_time);
        if (PlayerHitsAnyWall()) {
            game_over_ = true;
        }
    }

    engine_game_.FrameUpdate(ctx);
}

void Game::OnFrameRender(const VulkanEngine::Application::ApplicationContext& ctx) {
    engine_game_.FrameRender(ctx);
}

void Game::OnShutdown(VulkanEngine::Application::ApplicationContext& /*ctx*/) {
    imgui_draw_handle_ = {};
    engine_game_.Shutdown();
}

} // namespace Examples::InfiniteRunner::Game
