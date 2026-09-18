module;

#include <SDL3/SDL_keycode.h>
#include <cstdlib>
#include <logging/logging_macros.hpp>

module Examples.CustomPass.Game;

import std;

import logiface;

import VulkanEngine.GameEngine;
import VulkanEngine.GpuResources.MeshData;
import VulkanEngine.ShaderManager;
import Examples.CustomPass.Passes;

import Shaders.CustomPass.FullscreenVert;
import Shaders.CustomPass.CaptureFrag;
import Shaders.CustomPass.ToneMapFrag;
import Shaders.CustomPass.ExposureComp;

namespace Examples::CustomPass::Game {

CustomPassGame::CustomPassGame(const std::filesystem::path& executable_path)
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

CustomPassGame::~CustomPassGame() = default;

bool CustomPassGame::OnSetup(VulkanEngine::Application::ApplicationContext& ctx) {
    // 1. Engine subsystems. ImGui is enabled so the built-in ImGui pass exists
    // and the tonemap can order itself before it via the anchor.
    VulkanEngine::GameConfig config{};
    config.enable_imgui = true;
    config.renderer_config.clear_color = {0.05f, 0.05f, 0.06f, 1.0f};
    config.shader_data_dir = (exe_dir_ / "shaders").string();

    if (!engine_game_.Setup(ctx, config)) {
        return false;
    }

    // 2. Register this example's shaders through the engine path.
    auto& shader_mgr = engine_game_.GetContext().GetShaderManager();
    const auto fullscreen_vert =
        Shaders::CustomPass::FullscreenVert::Register(shader_mgr, config.shader_data_dir);
    const auto capture_frag =
        Shaders::CustomPass::CaptureFrag::Register(shader_mgr, config.shader_data_dir);
    const auto tonemap_frag =
        Shaders::CustomPass::ToneMapFrag::Register(shader_mgr, config.shader_data_dir);
    const auto exposure_comp =
        Shaders::CustomPass::ExposureComp::Register(shader_mgr, config.shader_data_dir);

    // The standard mesh pass still needs a shader pair; nothing is drawn with
    // it below except the fallback quad.
    const auto mesh_vert = engine_game_.GetContext().GetShaderIds().main_indir_vert;
    const auto mesh_frag = engine_game_.GetContext().GetShaderIds().standard_mesh_frag;
    if (!engine_game_.InitRenderer(ctx, mesh_vert, mesh_frag, &shader_mgr)) {
        return false;
    }

    // Watch this example's shader directory for hot reload.
    (void)engine_game_.AddShaderDirectory(config.shader_data_dir);

    // 3. A trivial scene (engine fallback quad) so the capture pass has content.
    auto quad = VulkanEngine::SceneLoader::ToMeshData(VulkanEngine::SceneLoader::CreateFallbackQuad());
    const std::uint32_t quad_id = engine_game_.GetMeshRegistry().Register(quad);

    auto& component_registry = engine_game_.GetContext().GetComponentRegistry();
    engine_game_.CreateCamera(component_registry);

    auto& entity = component_registry.CreateEntity();
    component_registry.AddComponent<VulkanEngine::Components::Transform>(entity);
    auto& mesh_ref = component_registry.AddComponent<VulkanEngine::Components::MeshReference>(entity);
    mesh_ref.loaded_mesh_id = quad_id;

    engine_game_.MarkSceneValid();

    // 4. Register the app passes through the public engine-managed API.
    pipeline_ = &engine_game_.GetRenderPipeline();

    const char* mode_env = std::getenv("CUSTOM_PASS_MODE");
    const std::string mode = mode_env != nullptr ? mode_env : "all";
    const bool want_capture = mode != "none";
    const bool want_compute = mode == "all";
    const bool want_tonemap = mode == "all" || mode == "capture_tonemap";

    if (want_capture) {
        auto capture = pipeline_->RegisterPass(
            std::make_unique<CapturePass>(fullscreen_vert, capture_frag));
        if (!capture.has_value()) {
            LOGIFACE_LOG(error, "CustomPassGame: failed to register capture pass");
            return false;
        }
        capture_handle_ = *capture;
    }
    if (want_compute) {
        auto exposure = pipeline_->RegisterPass(
            std::make_unique<ExposurePass>(exposure_comp));
        if (!exposure.has_value()) {
            LOGIFACE_LOG(error, "CustomPassGame: failed to register exposure pass");
            return false;
        }
        exposure_handle_ = *exposure;
        if (want_capture) {
            (void)pipeline_->AddDependency(capture_handle_, exposure_handle_);
        }
    }
    if (want_tonemap) {
        auto tonemap = pipeline_->RegisterPass(
            std::make_unique<ToneMapPass>(fullscreen_vert, tonemap_frag));
        if (!tonemap.has_value()) {
            LOGIFACE_LOG(error, "CustomPassGame: failed to register tonemap pass");
            return false;
        }
        tonemap_handle_ = *tonemap;
        if (want_compute) {
            (void)pipeline_->AddDependency(exposure_handle_, tonemap_handle_);
        }
    }

    // 5. Runtime enable/disable of the whole custom chain (F1).
    toggle_passes_ = ctx.input_system->BindAction("toggle-custom-passes",
        VulkanEngine::Input::InputBinding::Key(SDLK_F1));

    // 6. Quit action.
    ctx.quit_action_handle = ctx.input_system->BindAction("quit",
        VulkanEngine::Input::InputBinding::Key(SDLK_ESCAPE));

    return true;
}

void CustomPassGame::OnFrameUpdate(const VulkanEngine::Application::ApplicationContext& ctx) {
    if (ctx.input_system != nullptr && ctx.input_system->WasActionStarted(toggle_passes_) &&
        pipeline_ != nullptr) {
        passes_enabled_ = !passes_enabled_;
        (void)pipeline_->SetPassEnabled(capture_handle_, passes_enabled_);
        (void)pipeline_->SetPassEnabled(exposure_handle_, passes_enabled_);
        (void)pipeline_->SetPassEnabled(tonemap_handle_, passes_enabled_);
    }
    engine_game_.FrameUpdate(ctx);
}

void CustomPassGame::OnFrameRender(const VulkanEngine::Application::ApplicationContext& ctx) {
    engine_game_.FrameRender(ctx);
}

void CustomPassGame::OnShutdown(VulkanEngine::Application::ApplicationContext& /*ctx*/) {
    engine_game_.Shutdown();
}

} // namespace Examples::CustomPass::Game
