module;

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_RADIANS
#include <glm/vec4.hpp>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <filesystem>

#include <SDL3/SDL_keycode.h>
#include <imgui.h>
#include <vulkan/vulkan.hpp>

module App.Game;

import VulkanEngine.Game;
import App.Components.DemoInputComponent;

namespace App::Game {

DemoGame::DemoGame(const RenderMode render_mode, const std::filesystem::path& executable_path,
                   std::filesystem::path model_path,
                   std::filesystem::path texture_path)
    : render_mode_(render_mode)
    , exe_dir_(executable_path.parent_path())
    , model_path_(std::move(model_path))
    , texture_path_(std::move(texture_path)) {
    hooks_.on_setup = [this](VulkanEngine::Application::ApplicationContext& ctx) -> bool {
        return OnSetup(ctx);
    };
    hooks_.on_pre_input = [this](VulkanEngine::Application::ApplicationContext& ctx) {
        OnPreInput(ctx);
    };
    hooks_.should_filter_mouse_input = [this]() -> bool {
        return ShouldFilterMouseInput();
    };
    hooks_.should_filter_keyboard_input = [this]() -> bool {
        return ShouldFilterKeyboardInput();
    };
    hooks_.on_frame_update = [this](VulkanEngine::Application::ApplicationContext& ctx) {
        OnFrameUpdate(ctx);
    };
    hooks_.on_frame_render = [this](VulkanEngine::Application::ApplicationContext& ctx) {
        OnFrameRender(ctx);
    };
    hooks_.on_shutdown = [this](VulkanEngine::Application::ApplicationContext& ctx) {
        OnShutdown(ctx);
    };
}

DemoGame::~DemoGame() = default;

bool DemoGame::OnSetup(VulkanEngine::Application::ApplicationContext& ctx) {
    // 1. Configure and init engine subsystems
    VulkanEngine::Game::GameConfig config{};
    config.shader_dir = SHADER_DIR;
    config.enable_imgui = true;
    config.renderer_config.clear_color = {0.1f, 0.1f, 0.1f, 1.0f};

    switch (render_mode_) {
        case RenderMode::Normals:
            config.fragment_shader_file = "normals.frag.spv";
            break;
        case RenderMode::NoTextures:
            config.fragment_shader_file = "solid.frag.spv";
            break;
        default:
            config.fragment_shader_file = "standard_mesh.frag.spv";
            break;
    }

    if (!engine_game_.Setup(ctx, config)) {
        return false;
    }

    // 2. Load meshes
    std::vector<VulkanEngine::SceneLoader::LoadedMeshData> meshes;
    if (!model_path_.empty()) {
        meshes = engine_game_.LoadMeshes({model_path_});
    } else {
        meshes = engine_game_.LoadMeshDirectory(exe_dir_ / "models");
    }

    // 3. Finalize scene (creates renderer, techniques, uploads to GPU, resolves materials)
    const auto& scene = engine_game_.FinalizeScene(ctx, meshes);

    // 5. Create camera
    auto& backend = ctx.bootstrap->GetBackend();
    engine_game_.CreateCamera(backend.GetComponentRegistry());

    // 6. Create game entities
    for (size_t i = 0; i < scene.meshes.size(); ++i) {
        auto& entity = backend.GetComponentRegistry().CreateEntity();
        backend.GetComponentRegistry().AddComponent<VulkanEngine::Components::Transform>(entity);

        auto& mesh_ref = backend.GetComponentRegistry().AddComponent<VulkanEngine::Components::MeshReference>(entity);
        mesh_ref.first_submesh = scene.meshes[i].first_submesh_index;
        mesh_ref.submesh_count = scene.meshes[i].submesh_count;
        mesh_ref.index_buffer_index = static_cast<uint8_t>(scene.index_allocation.buffer_index);

        if (i == 0) {
            backend.GetComponentRegistry().AddComponent<App::Components::DemoInputComponent>(entity, ctx.input_system);
        }
    }

    backend.GetComponentRegistry().InitializeAllComponents();

    // 7. Bind quit action
    ctx.quit_action_handle = ctx.input_system->BindAction("quit",
        VulkanEngine::Input::InputBinding::Key(SDLK_ESCAPE));

    return true;
}

void DemoGame::OnPreInput(VulkanEngine::Application::ApplicationContext& /*ctx*/) {
}

bool DemoGame::ShouldFilterMouseInput() {
    return ImGui::GetIO().WantCaptureMouse;
}

bool DemoGame::ShouldFilterKeyboardInput() {
    return ImGui::GetIO().WantCaptureKeyboard;
}

void DemoGame::OnFrameUpdate(const VulkanEngine::Application::ApplicationContext& ctx) {
    engine_game_.FrameUpdate(ctx);
}

void DemoGame::OnFrameRender(const VulkanEngine::Application::ApplicationContext& ctx) {
    engine_game_.FrameRender(ctx);
}

void DemoGame::OnShutdown(VulkanEngine::Application::ApplicationContext& /*ctx*/) {
    engine_game_.Shutdown();
}

} // namespace App::Game
