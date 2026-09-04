module;
#include <SDL3/SDL_keycode.h>
#include <logging/logging_macros.hpp>

module Examples.Minimal.Game;

import std;

import logiface;

import VulkanEngine.GameEngine;
import VulkanEngine.GpuResources.MeshData;
import Shaders.Engine.StandardMeshFrag;
import Shaders.Minimal.CheckerFrag;
import VulkanEngine.ShaderManager;

namespace Examples::Minimal::Game {

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

bool Game::OnSetup(VulkanEngine::Application::ApplicationContext& ctx) {
    // 1. Configure and init engine subsystems
    VulkanEngine::GameConfig config{};
    config.enable_imgui = false;
    config.renderer_config.clear_color = {0.1f, 0.1f, 0.1f, 1.0f};
    config.shader_data_dir = (exe_dir_ / "shaders").string();

    if (!engine_game_.Setup(ctx, config)) {
        return false;
    }

    // 2. Register the app fragment shader and init the renderer
    auto& shader_mgr = engine_game_.GetContext().GetShaderManager();
    auto frag_id = Shaders::Minimal::CheckerFrag::Register(shader_mgr, config.shader_data_dir);
    auto vert_id = engine_game_.GetContext().GetShaderIds().main_indir_vert;
    if (!engine_game_.InitRenderer(ctx, vert_id, frag_id, &shader_mgr)) {
        return false;
    }

    // 3. Load a mesh (engine built-in fallback quad — no assets needed)
    auto quad = VulkanEngine::SceneLoader::ToMeshData(VulkanEngine::SceneLoader::CreateFallbackQuad());
    const std::uint32_t quad_id = engine_game_.GetMeshRegistry().Register(quad);

    engine_game_.MarkSceneValid();

    // 4. Camera
    auto& component_registry = engine_game_.GetContext().GetComponentRegistry();
    engine_game_.CreateCamera(component_registry);

    // 5. Entity with the mesh
    auto& entity = component_registry.CreateEntity();
    component_registry.AddComponent<VulkanEngine::Components::Transform>(entity);
    auto& mesh_ref = component_registry.AddComponent<VulkanEngine::Components::MeshReference>(entity);
    mesh_ref.loaded_mesh_id = quad_id;

    // 6. Bind quit action
    ctx.quit_action_handle = ctx.input_system->BindAction("quit",
        VulkanEngine::Input::InputBinding::Key(SDLK_ESCAPE));

    return true;
}

void Game::OnFrameUpdate(const VulkanEngine::Application::ApplicationContext& ctx) {
    engine_game_.FrameUpdate(ctx);
}

void Game::OnFrameRender(const VulkanEngine::Application::ApplicationContext& ctx) {
    engine_game_.FrameRender(ctx);
}

void Game::OnShutdown(VulkanEngine::Application::ApplicationContext& /*ctx*/) {
    engine_game_.Shutdown();
}

} // namespace Examples::Minimal::Game
