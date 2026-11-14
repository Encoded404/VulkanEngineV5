module;

#include <glm/glm.hpp>

// The shared explorer application: owns the engine lifecycle, the camera, the
// scene list and the options panels. An example becomes an explorer by handing
// this class a SceneCatalog; it never touches hooks, ImGui or the camera itself.
export module Examples.Explorer.App;

import std;

import VulkanEngine.GameEngine;
import VulkanEngine.GplPolicy;
import VulkanShared.CallbackList;

import Examples.Explorer.Camera;
import Examples.Explorer.Math;
import Examples.Explorer.Options;
import Examples.Explorer.Scene;

export namespace Examples::Explorer {

struct ExplorerConfig {
    std::string title{"VulkanEngineV5 Scene Explorer"};
    std::filesystem::path exe_dir{};
    std::filesystem::path asset_dir{};
    std::string initial_scene{};
    std::string initial_action{};
    CameraMode initial_mode = CameraMode::Showcase;
    VulkanEngine::ShaderSystem::GplPolicy gpl_policy = VulkanEngine::ShaderSystem::GplPolicy::Auto;
    VulkanEngine::ShaderSystem::GplStructurePolicy gpl_structure =
        VulkanEngine::ShaderSystem::GplStructurePolicy::Auto;
};

class ExplorerApp {
public:
    ExplorerApp(ExplorerConfig config, SceneCatalog catalog);
    ~ExplorerApp();

    ExplorerApp(const ExplorerApp&) = delete;
    ExplorerApp& operator=(const ExplorerApp&) = delete;

    [[nodiscard]] const VulkanEngine::Application::ApplicationHooks& GetHooks() const noexcept {
        return hooks_;
    }
    [[nodiscard]] ExplorerConfig& Config() noexcept { return config_; }
    [[nodiscard]] const SceneCatalog& Catalog() const noexcept { return catalog_; }

private:
    bool OnSetup(VulkanEngine::Application::ApplicationContext& ctx);
    bool BuildScene(VulkanEngine::Modules::ModuleContext& mctx);
    void OnPreInput(VulkanEngine::Application::ApplicationContext& ctx);
    void OnFixedUpdate(const VulkanEngine::Application::ApplicationContext& ctx);
    void OnFrameUpdate(const VulkanEngine::Application::ApplicationContext& ctx);
    void OnFrameRender(const VulkanEngine::Application::ApplicationContext& ctx);
    void OnShutdown(VulkanEngine::Application::ApplicationContext& ctx);

    bool ShouldFilterMouseInput();
    bool ShouldFilterKeyboardInput();

    // ── Panels ──────────────────────────────────────────────────────────
    void DrawPanels();
    void DrawSceneList();
    void DrawControlPanel();
    void DrawPathControls();

    // ── Scene lifecycle ─────────────────────────────────────────────────
    bool LoadScene(std::size_t index, VulkanEngine::Application::ApplicationContext& ctx);
    void UnloadScene(VulkanEngine::Application::ApplicationContext& ctx);
    void ApplyPendingSceneChange(VulkanEngine::Application::ApplicationContext& ctx);
    [[nodiscard]] SceneContext MakeSceneContext(VulkanEngine::Application::ApplicationContext& ctx);
    [[nodiscard]] SceneDefinition* CurrentDefinition();

    // ── Input ───────────────────────────────────────────────────────────
    void GatherCameraInput(const VulkanEngine::Application::ApplicationContext& ctx,
                           CameraInput& out) const;
    void ApplyHotkeys(const VulkanEngine::Application::ApplicationContext& ctx);

    ExplorerConfig config_{};
    SceneCatalog catalog_{};
    VulkanEngine::Application::ApplicationHooks hooks_{};

    VulkanShared::ScopedHandle<bool(VulkanEngine::Application::ApplicationContext&)> setup_token_{};
    VulkanShared::ScopedHandle<void(VulkanEngine::Application::ApplicationContext&)> pre_input_token_{};
    VulkanShared::ScopedHandle<void(VulkanEngine::Application::ApplicationContext&)> fixed_update_token_{};
    VulkanShared::ScopedHandle<void(VulkanEngine::Application::ApplicationContext&)> frame_update_token_{};
    VulkanShared::ScopedHandle<void(VulkanEngine::Application::ApplicationContext&)> frame_render_token_{};
    VulkanShared::ScopedHandle<void(VulkanEngine::Application::ApplicationContext&)> shutdown_token_{};
    VulkanShared::ScopedHandle<void()> imgui_draw_token_{};

    ExplorerCamera camera_{};
    OptionsUi camera_options_{};
    OptionsUi scene_options_{};

    std::size_t current_scene_ = std::numeric_limits<std::size_t>::max();
    std::optional<std::size_t> pending_scene_{};
    bool reload_requested_ = false;
    bool first_scene_loaded_ = false;
    std::vector<VulkanEngine::EntityId> scene_entities_{};
    std::string scene_error_{};
    std::string scene_notice_{};

    // Declared last: its module lambdas capture `this`, so the engine must be
    // destroyed before the state those lambdas read.
    VulkanEngine::GameEngine engine_game_{
        VulkanEngine::EngineFeatures{.default_renderer = true, .imgui = true}};
};

} // namespace Examples::Explorer
