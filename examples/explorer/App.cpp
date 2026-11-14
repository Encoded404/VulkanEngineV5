module;

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_mouse.h>

#include <imgui.h>

#include <logging/logging_macros.hpp>

module Examples.Explorer.App;

import std;

import logiface;

import VulkanEngine.GameEngine;
import VulkanEngine.GplPolicy;
import VulkanShared.CallbackList;

import Examples.Explorer.Camera;
import Examples.Explorer.Math;
import Examples.Explorer.Options;
import Examples.Explorer.Scene;

namespace Examples::Explorer {

namespace {

bool Held(const VulkanEngine::Input::RawInputState& raw, std::uint32_t key) {
    return raw.held_keys.contains(static_cast<std::int32_t>(key));
}

bool Pressed(const VulkanEngine::Input::RawInputState& raw, std::uint32_t key) {
    return raw.pressed_keys.contains(static_cast<std::int32_t>(key));
}

} // namespace

ExplorerApp::ExplorerApp(ExplorerConfig config, SceneCatalog catalog)
    : config_(std::move(config)), catalog_(std::move(catalog)) {
    engine_game_.Modules().Add(VulkanEngine::Modules::EngineModuleDesc{
        .stage = VulkanEngine::Modules::Stage::Scene,
        .label = "app.explorer_scene",
        .startup = [this](VulkanEngine::Modules::ModuleContext& mctx) -> bool {
            return BuildScene(mctx);
        },
    });

    setup_token_ = hooks_.on_setup.Register(
        [this](VulkanEngine::Application::ApplicationContext& ctx) -> bool { return OnSetup(ctx); });
    pre_input_token_ = hooks_.on_pre_input.Register(
        [this](VulkanEngine::Application::ApplicationContext& ctx) { OnPreInput(ctx); });
    hooks_.should_filter_mouse_input = [this]() -> bool { return ShouldFilterMouseInput(); };
    hooks_.should_filter_keyboard_input = [this]() -> bool { return ShouldFilterKeyboardInput(); };
    fixed_update_token_ = hooks_.on_fixed_update.Register(
        [this](const VulkanEngine::Application::ApplicationContext& ctx) { OnFixedUpdate(ctx); });
    frame_update_token_ = hooks_.on_frame_update.Register(
        [this](const VulkanEngine::Application::ApplicationContext& ctx) { OnFrameUpdate(ctx); });
    frame_render_token_ = hooks_.on_frame_render.Register(
        [this](const VulkanEngine::Application::ApplicationContext& ctx) { OnFrameRender(ctx); });
    shutdown_token_ = hooks_.on_shutdown.Register(
        [this](VulkanEngine::Application::ApplicationContext& ctx) { OnShutdown(ctx); });
}

ExplorerApp::~ExplorerApp() = default;

bool ExplorerApp::OnSetup(VulkanEngine::Application::ApplicationContext& ctx) {
    VulkanEngine::GameConfig config{};
    config.renderer_config.clear_color = {0.035f, 0.04f, 0.05f, 1.0f};
    config.shader_data_dir = (config_.exe_dir / "shaders").string();
    config.gpl_policy = config_.gpl_policy;
    config.gpl_structure = config_.gpl_structure;
    // Neutral studio lighting: the explorer itself is scene-agnostic, and a
    // scene that wants a different rig uploads one from its update callback.
    config.lighting.sun_direction = {0.372f, -0.744f, 0.558f};
    config.lighting.sun_color = {1.0f, 0.96f, 0.9f, 3.0f};
    config.lighting.ambient_color = {0.05f, 0.06f, 0.075f, 1.0f};

    if (!engine_game_.Setup(ctx, config)) {
        return false;
    }

    auto& shader_mgr = engine_game_.GetContext().GetShaderManager();
    const auto vert_id = engine_game_.GetContext().GetShaderIds().main_indir_vert;
    const auto frag_id = engine_game_.GetContext().GetShaderIds().standard_mesh_frag;
    if (!engine_game_.InitRenderer(ctx, VulkanEngine::ShaderSystem::RendererShaders{
                                              .vert = vert_id, .frag = frag_id, .manager = &shader_mgr})) {
        return false;
    }

    ctx.quit_action_handle =
        ctx.input_system->BindAction("quit", VulkanEngine::Input::InputBinding::Key(SDLK_ESCAPE));
    return true;
}

bool ExplorerApp::BuildScene(VulkanEngine::Modules::ModuleContext& mctx) {
    auto& ctx = mctx.App();

    engine_game_.CreateCamera(mctx.Registry());
    camera_.SetMode(config_.initial_mode);
    if (auto* camera = engine_game_.GetCamera()) {
        camera_.ApplyTo(*camera);
    }

    camera_options_.Clear();
    camera_.BuildOptions(camera_options_);

    if (auto* imgui = engine_game_.GetImGuiSystem()) {
        imgui_draw_token_ = imgui->draw_callbacks.Register([this] { DrawPanels(); });
    }

    if (catalog_.empty()) {
        LOGIFACE_LOG(error, "Explorer: the scene catalog is empty; nothing to show");
        engine_game_.MarkSceneValid();
        scene_error_ = "The scene catalog is empty. Nothing was registered by the host "
                       "application.";
        return true;
    }

    std::size_t initial = 0;
    if (!config_.initial_scene.empty()) {
        const auto found = std::ranges::find_if(catalog_, [this](const SceneDefinition& scene) {
            return scene.id == config_.initial_scene;
        });
        if (found != catalog_.end()) {
            initial = static_cast<std::size_t>(std::distance(catalog_.begin(), found));
        } else {
            LOGIFACE_LOG(warn, "Explorer: unknown --scene '" + config_.initial_scene +
                                   "'; falling back to '" + catalog_.front().id + "'");
        }
    }

    if (!LoadScene(initial, ctx)) {
        LOGIFACE_LOG(warn, "Explorer: initial scene failed to load: " + scene_error_);
    }

    engine_game_.MarkSceneValid();
    return true;
}

SceneContext ExplorerApp::MakeSceneContext(
    VulkanEngine::Application::ApplicationContext& ctx) {
    SceneContext scene_ctx{};
    scene_ctx.app = &ctx;
    scene_ctx.engine = &engine_game_;
    scene_ctx.asset_dir = config_.asset_dir;
    scene_ctx.request_reload = [this] { reload_requested_ = true; };
    scene_ctx.initial_action = config_.initial_action;
    return scene_ctx;
}

SceneDefinition* ExplorerApp::CurrentDefinition() {
    if (current_scene_ >= catalog_.size()) return nullptr;
    return &catalog_[current_scene_];
}

bool ExplorerApp::LoadScene(std::size_t index,
                            VulkanEngine::Application::ApplicationContext& ctx) {
    if (catalog_.empty()) return false;
    index = std::min(index, catalog_.size() - 1);

    UnloadScene(ctx);

    current_scene_ = index;
    SceneDefinition& definition = catalog_[index];

    bool ok = true;
    SceneBuildResult result{};
    if (definition.load) {
        SceneContext scene_ctx = MakeSceneContext(ctx);
        result = definition.load(scene_ctx);
        ok = result.ok;
    } else {
        result.ok = false;
        result.error = "This scene has no load callback.";
        ok = false;
    }

    scene_entities_ = std::move(result.entities);
    scene_error_ = std::move(result.error);
    scene_notice_ = std::move(result.notice);

    // The launch mode applies to the first scene; later switches keep whatever
    // mode the user is in (Path falls back by itself when the next scene has no
    // paths).
    const CameraMode wanted_mode =
        first_scene_loaded_ ? camera_.Mode() : config_.initial_mode;
    camera_.SetPaths(definition.paths);
    camera_.SetMode(wanted_mode);
    camera_.SetBounds(result.bounds);
    camera_.FrameBounds();
    if (auto* camera = engine_game_.GetCamera()) {
        camera_.ApplyTo(*camera);
    }

    scene_options_.Clear();
    if (definition.options) {
        definition.options(scene_options_);
    }

    first_scene_loaded_ = true;
    if (!ok) {
        LOGIFACE_LOG(error, "Explorer: scene '" + definition.id + "' failed: " + scene_error_);
    }
    return ok;
}

void ExplorerApp::UnloadScene(VulkanEngine::Application::ApplicationContext& ctx) {
    if (current_scene_ < catalog_.size() && catalog_[current_scene_].unload) {
        SceneContext scene_ctx = MakeSceneContext(ctx);
        catalog_[current_scene_].unload(scene_ctx);
    }

    auto& registry = engine_game_.GetContext().component_registry;
    for (const VulkanEngine::EntityId id : scene_entities_) {
        registry.DestroyEntity(id);
    }
    scene_entities_.clear();
    scene_options_.Clear();
    scene_error_.clear();
    scene_notice_.clear();
    current_scene_ = std::numeric_limits<std::size_t>::max();
}

void ExplorerApp::ApplyPendingSceneChange(
    VulkanEngine::Application::ApplicationContext& ctx) {
    // Nothing to do on the overwhelming majority of frames: return before any
    // device-wide work. (A waitIdle here unconditionally cost ~70 ms/frame and
    // serialized the whole pipeline.)
    if (!pending_scene_.has_value() && !reload_requested_) {
        return;
    }

    // Tearing a scene down releases its materials, meshes and bindless texture
    // slots. Doing that while the async uploader still owns work for them races
    // the upload path, so a pending switch waits for the uploader to drain
    // (usually sub-frame on a GPU, longer while a heavy asset is still decoding
    // on a software device).
    if (engine_game_.HasPendingTextureUploads()) {
        return;
    }

    // Quiesce the device only on the frame that actually swaps scenes: the
    // uploader reuses its staging heap, and the old scene's GPU reads must be
    // finished before its resources are released.
    if (ctx.bootstrap != nullptr) {
        static_cast<void>(ctx.bootstrap->GetBackend().GetDevice().waitIdle());
    }

    if (pending_scene_.has_value()) {
        const std::size_t index = *pending_scene_;
        pending_scene_.reset();
        reload_requested_ = false;
        LoadScene(index, ctx);
        return;
    }
    if (reload_requested_) {
        reload_requested_ = false;
        if (current_scene_ < catalog_.size()) {
            const std::size_t index = current_scene_;
            LoadScene(index, ctx);
        }
    }
}

void ExplorerApp::OnPreInput(VulkanEngine::Application::ApplicationContext& ctx) {
    // Scene swaps rebuild materials and textures, which uses the staging pool's
    // immediate path; doing it at the frame boundary keeps that out of a render.
    ApplyPendingSceneChange(ctx);
}

void ExplorerApp::GatherCameraInput(
    const VulkanEngine::Application::ApplicationContext& ctx, CameraInput& out) const {
    const auto* input = ctx.input_system;
    if (input == nullptr) return;

    const ImGuiIO& io = ImGui::GetIO();
    const auto& raw = input->GetRawState();

    out.look_drag = !io.WantCaptureMouse && raw.held_mouse_buttons.contains(SDL_BUTTON_RIGHT);
    out.orbit_drag = !io.WantCaptureMouse && raw.held_mouse_buttons.contains(SDL_BUTTON_LEFT);
    out.pan_drag = !io.WantCaptureMouse && raw.held_mouse_buttons.contains(SDL_BUTTON_MIDDLE);
    out.mouse_delta = glm::vec2{raw.mouse_delta_x, raw.mouse_delta_y};
    // Scrolling over a panel must not also zoom the camera.
    out.wheel = io.WantCaptureMouse ? 0.0f : raw.wheel_y;

    if (io.WantCaptureKeyboard) return;

    float strafe = 0.0f;
    float forward = 0.0f;
    if (Held(raw, SDLK_W) || Held(raw, SDLK_UP)) forward += 1.0f;
    if (Held(raw, SDLK_S) || Held(raw, SDLK_DOWN)) forward -= 1.0f;
    if (Held(raw, SDLK_D) || Held(raw, SDLK_RIGHT)) strafe += 1.0f;
    if (Held(raw, SDLK_A) || Held(raw, SDLK_LEFT)) strafe -= 1.0f;
    out.move = glm::vec2{strafe, forward};

    float lift = 0.0f;
    if (Held(raw, SDLK_E) || Held(raw, SDLK_SPACE)) lift += 1.0f;
    if (Held(raw, SDLK_Q) || Held(raw, SDLK_C)) lift -= 1.0f;
    out.lift = lift;
    out.boost = Held(raw, SDLK_LSHIFT) || Held(raw, SDLK_RSHIFT);
}

void ExplorerApp::ApplyHotkeys(
    const VulkanEngine::Application::ApplicationContext& ctx) {
    const auto* input = ctx.input_system;
    if (input == nullptr) return;
    if (ImGui::GetIO().WantCaptureKeyboard) return;
    const auto& raw = input->GetRawState();

    if (Pressed(raw, SDLK_1)) camera_.SetMode(CameraMode::Freefly);
    if (Pressed(raw, SDLK_2)) camera_.SetMode(CameraMode::Showcase);
    if (Pressed(raw, SDLK_3)) camera_.SetMode(CameraMode::Path);
    if (Pressed(raw, SDLK_F)) camera_.FrameBounds();
}

void ExplorerApp::OnFixedUpdate(const VulkanEngine::Application::ApplicationContext& ctx) {
    engine_game_.FixedUpdate(ctx);
}

void ExplorerApp::OnFrameUpdate(const VulkanEngine::Application::ApplicationContext& ctx) {
    ApplyHotkeys(ctx);

    CameraInput input{};
    GatherCameraInput(ctx, input);
    camera_.Update(input, ctx.frame.delta_time);
    if (auto* camera = engine_game_.GetCamera()) {
        camera_.ApplyTo(*camera);
    }

    if (current_scene_ < catalog_.size()) {
        SceneDefinition& definition = catalog_[current_scene_];
        if (definition.update) {
            // The frame hooks receive the context const, but RunApplication owns
            // it mutably for the whole run and scene updates may hand it to
            // upload helpers, so the constness here is the hook signature's.
            SceneContext scene_ctx = MakeSceneContext(
                const_cast<VulkanEngine::Application::ApplicationContext&>(ctx));
            definition.update(scene_ctx, ctx.frame.delta_time);
        }
    }

    engine_game_.FrameUpdate(ctx);
}

void ExplorerApp::OnFrameRender(const VulkanEngine::Application::ApplicationContext& ctx) {
    engine_game_.FrameRender(ctx);
}

void ExplorerApp::OnShutdown(VulkanEngine::Application::ApplicationContext& /*ctx*/) {
    imgui_draw_token_ = {};
    engine_game_.Shutdown();
}

bool ExplorerApp::ShouldFilterMouseInput() {
    return ImGui::GetIO().WantCaptureMouse;
}

bool ExplorerApp::ShouldFilterKeyboardInput() {
    return ImGui::GetIO().WantCaptureKeyboard;
}

void ExplorerApp::DrawPanels() {
    DrawSceneList();
    DrawControlPanel();
}

void ExplorerApp::DrawSceneList() {
    ImGui::SetNextWindowPos(ImVec2(12.0f, 12.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(330.0f, 520.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Scenes", nullptr, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    for (std::size_t i = 0; i < catalog_.size(); ++i) {
        const SceneDefinition& scene = catalog_[i];
        const bool selected = (i == current_scene_);
        if (ImGui::Selectable(scene.name.c_str(), selected, 0, ImVec2(0.0f, 0.0f))) {
            pending_scene_ = i;
        }
        if (ImGui::IsItemHovered() && !scene.summary.empty()) {
            ImGui::SetItemTooltip("%.*s", static_cast<int>(scene.summary.size()),
                                  scene.summary.data());
        }
    }

    ImGui::Separator();
    if (SceneDefinition* scene = CurrentDefinition(); scene != nullptr) {
        ImGui::TextWrapped("%.*s", static_cast<int>(scene->summary.size()),
                           scene->summary.data());
        if (!scene->license.empty()) {
            ImGui::TextDisabled("%s", scene->license.c_str());
        }
        if (!scene->attribution.empty()) {
            ImGui::TextWrapped("%.*s", static_cast<int>(scene->attribution.size()),
                               scene->attribution.data());
        }
        if (!scene->source.empty()) {
            ImGui::TextDisabled("%.*s", static_cast<int>(scene->source.size()),
                                scene->source.data());
        }
        if (!camera_.HasPaths()) {
            ImGui::TextDisabled("No camera paths in this scene.");
        }
    }
    ImGui::End();
}

void ExplorerApp::DrawPathControls() {
    if (!camera_.HasPaths()) return;

    ImGui::SeparatorText("Camera path");
    std::vector<const char*> names;
    names.reserve(camera_.Paths().size());
    for (const CameraPath& path : camera_.Paths()) names.push_back(path.name.c_str());

    int index = std::max(camera_.PathIndex(), 0);
    if (ImGui::Combo("Path", &index, names.data(), static_cast<int>(names.size()))) {
        camera_.SelectPath(index);
        camera_.SetMode(CameraMode::Path);
    }

    const bool playing = camera_.PathPlaying();
    if (ImGui::Button(playing ? "Pause" : "Play")) {
        if (!playing && camera_.PathPosition() >= 1.0f && !camera_.PathLoops()) {
            camera_.SetPathPosition(0.0f);
        }
        camera_.SetPathPlaying(!playing);
    }
    ImGui::SameLine();
    bool loop = camera_.PathLoops();
    if (ImGui::Checkbox("Loop", &loop)) camera_.SetPathLoops(loop);

    float speed = camera_.PathSpeed();
    if (ImGui::SliderFloat("Speed", &speed, 0.1f, 40.0f, "%.2f u/s")) {
        camera_.SetPathSpeed(speed);
    }

    float position = camera_.PathPosition();
    if (ImGui::SliderFloat("Position", &position, 0.0f, 1.0f, "%.3f")) {
        camera_.SetPathPlaying(false);
        camera_.SetPathPosition(position);
    }

    const auto path_index = static_cast<std::size_t>(std::max(camera_.PathIndex(), 0));
    if (path_index < camera_.Paths().size()) {
        ImGui::TextDisabled("Length: %.1f units", ApproximatePathLength(camera_.Paths()[path_index]));
    }
}

void ExplorerApp::DrawControlPanel() {
    ImGui::SetNextWindowPos(ImVec2(360.0f, 12.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(390.0f, 620.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Explorer", nullptr, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    int mode = static_cast<int>(camera_.Mode());
    if (ImGui::RadioButton("Freefly", mode == static_cast<int>(CameraMode::Freefly))) {
        camera_.SetMode(CameraMode::Freefly);
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Showcase", mode == static_cast<int>(CameraMode::Showcase))) {
        camera_.SetMode(CameraMode::Showcase);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!camera_.HasPaths());
    if (ImGui::RadioButton("Path", mode == static_cast<int>(CameraMode::Path))) {
        camera_.SetMode(CameraMode::Path);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Frame")) camera_.FrameBounds();

    ImGui::TextDisabled("WASD move  Q/E down/up  RMB look  LMB orbit  MMB pan  wheel zoom  "
                        "1/2/3 mode  F frame");

    if (!scene_notice_.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", scene_notice_.c_str());
        ImGui::PopTextWrapPos();
    }

    if (!scene_error_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.35f, 1.0f));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(scene_error_.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }

    ImGui::SeparatorText("Scene options");
    if (scene_options_.Empty()) {
        ImGui::TextDisabled("This scene declares no options.");
    } else {
        scene_options_.Draw();
    }

    if (camera_.Mode() == CameraMode::Path) {
        DrawPathControls();
    }

    ImGui::SeparatorText("View");
    camera_options_.Draw();

    ImGui::Separator();
    ImGui::TextDisabled("%.0f fps", static_cast<double>(ImGui::GetIO().Framerate));
    ImGui::End();
}

} // namespace Examples::Explorer
