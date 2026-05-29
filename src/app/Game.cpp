module;

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_RADIANS
#include <glm/vec4.hpp> // NOLINT(misc-include-cleaner)
#include <glm/gtc/quaternion.hpp> // NOLINT(misc-include-cleaner)
#include <string>
#include <utility>
#include <vector>
#include <filesystem>

#include <SDL3/SDL_keycode.h>
#include <imgui.h>
#include <vulkan/vulkan.hpp>

module App.Game;

import VulkanEngine.Game;
import App.Components.SimpleControllerComponent;
import App.Components.TransformControlComponent;

namespace App::Game {

DemoGame::DemoGame(const RenderMode render_mode, const std::filesystem::path& executable_path,
                   std::filesystem::path model_path,
                   std::filesystem::path texture_path)
    : render_mode_(render_mode)
    , exe_dir_(executable_path.parent_path())
    , model_path_(std::move(model_path))
    , texture_path_(std::move(texture_path)) {
    setup_token_ = hooks_.on_setup.Register([this](VulkanEngine::Application::ApplicationContext& ctx) -> bool {
        return OnSetup(ctx);
    });
    pre_input_token_ = hooks_.on_pre_input.Register([this](VulkanEngine::Application::ApplicationContext& ctx) {
        OnPreInput(ctx);
    });
    hooks_.should_filter_mouse_input = [this]() -> bool {
        return ShouldFilterMouseInput();
    };
    hooks_.should_filter_keyboard_input = [this]() -> bool {
        return ShouldFilterKeyboardInput();
    };
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

    // 2. Init renderer, technique, and fallback material
    if (!engine_game_.InitRenderer(ctx)) {
        return false;
    }

    // 3. Create a custom material for the viking room
    const uint32_t tex_slot = engine_game_.LoadTexture(ctx, exe_dir_ / "textures" / "viking_room.png");
    const auto viking_mat_id = VulkanEngine::MaterialManager::MaterialManager::Get().RegisterMaterial({
        .technique_id = engine_game_.GetMainTechniqueId(),
        .texture_slot = VulkanEngine::BindlessManager::TextureSlot{static_cast<uint16_t>(tex_slot)}
    });

    // 4. Load meshes explicitly with material bindings
    std::vector<VulkanEngine::SceneLoader::LoadedMeshData> meshes;
    const std::vector<VulkanEngine::SceneLoader::MaterialId> viking_bindings = {viking_mat_id};

    auto viking_mesh = VulkanEngine::SceneLoader::SceneLoader::LoadMeshFromFilePath(
        exe_dir_ / "models" / "viking_room.obj", &viking_bindings);
    meshes.push_back(std::move(viking_mesh));

    auto monkey_mesh = VulkanEngine::SceneLoader::SceneLoader::LoadMeshFromFilePath(
        exe_dir_ / "models" / "simple-monkey.bin", nullptr);
    meshes.push_back(std::move(monkey_mesh));

    // 5. Upload scene to GPU
    const auto& scene = engine_game_.UploadScene(ctx, meshes);

    // 6. Create camera
    auto& backend = ctx.bootstrap->GetBackend();
    engine_game_.CreateCamera(backend.GetComponentRegistry());

    // 7. Create game entities
    for (size_t i = 0; i < scene.meshes.size(); ++i) {
        auto& entity = backend.GetComponentRegistry().CreateEntity();
        backend.GetComponentRegistry().AddComponent<VulkanEngine::Components::Transform>(entity);

        auto& mesh_ref = backend.GetComponentRegistry().AddComponent<VulkanEngine::Components::MeshReference>(entity);
        mesh_ref.first_submesh = scene.meshes[i].first_submesh_index;
        mesh_ref.submesh_count = scene.meshes[i].submesh_count;
        mesh_ref.index_buffer_index = static_cast<uint8_t>(scene.index_allocation.buffer_index);

        if (i == 0) {
            auto& debug_comp = backend.GetComponentRegistry().AddComponent<App::Components::TransformControlComponent>(entity);
            debug_comp.position = glm::vec3{0.0f, 0.0f, 0.0f};
            auto* mesh_ref_comp = entity.GetComponent<VulkanEngine::Components::MeshReference>();
            if (mesh_ref_comp && mesh_ref_comp->submesh_count > 0) {
                const auto& sm = scene.submeshes[mesh_ref_comp->first_submesh];
                debug_comp.material_id = sm.material_id;
                if (debug_comp.material_id.has_value()) {
                    auto& mat_def = VulkanEngine::MaterialManager::MaterialManager::Get().GetMaterial(debug_comp.material_id.value());
                    debug_comp.texture_slot = mat_def.texture_slot.value;
                }
            }
        } else if (i == 1) {
            backend.GetComponentRegistry().AddComponent<App::Components::SimpleControllerComponent>(entity, ctx.input_system);
        }
    }

    backend.GetComponentRegistry().InitializeAllComponents();

    // 9. Register ImGui debug UI
    auto* imgui = engine_game_.GetImGuiSystem();
    if (imgui) {
        imgui_draw_handle_ = imgui->draw_callbacks.Register([&registry = backend.GetComponentRegistry()]() {
            auto debug_comps = registry.GetAll<App::Components::TransformControlComponent>();
            if (debug_comps.empty()) return;

            for (auto* dc : debug_comps) {
                ImGui::Begin("Transform Control", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

                ImGui::DragFloat3("Position", &dc->position.x, 0.1f);

                ImGui::SeparatorText("Texture");
                ImGui::DragInt("Slot", &dc->texture_slot, 1, 0, 255);

                ImGui::SeparatorText("Rotation");
                constexpr const char* modes[] = {"Euler (vec3)", "Quaternion (vec4)"}; // NOLINT(modernize-avoid-c-arrays)
                int mode = static_cast<int>(dc->rotation_mode);
                if (ImGui::Combo("Mode", &mode, modes, 2)) {
                    dc->rotation_mode = static_cast<App::Components::RotationMode>(mode);
                }
                if (dc->rotation_mode == App::Components::RotationMode::Euler) {
                    ImGui::DragFloat3("Euler (deg)", &dc->rotation_euler.x, 1.0f);
                } else {
                    ImGui::DragFloat4("Quaternion", &dc->rotation_quat.x, 0.01f);
                }

                ImGui::End();
            }
        });
    }

    // 9. Bind quit action
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
    imgui_draw_handle_ = {};
    engine_game_.Shutdown();
}

} // namespace App::Game
