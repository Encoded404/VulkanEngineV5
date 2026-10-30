module;

export module Examples.CustomPass.Game;

import std;

export import VulkanEngine.GameEngine;
import VulkanShared.CallbackList;
import VulkanEngine.ShaderManager;

export namespace Examples::CustomPass::Game {

// Demonstrates the app render-pass API: three passes (a graphics capture, a
// compute exposure estimate, and a graphics tonemap) registered through
// RenderPipeline::RegisterPass with engine-owned pipelines and descriptors,
// ordered around the built-in MainPass and ImGui anchors, and toggled at
// runtime.
class CustomPassGame {
public:
    explicit CustomPassGame(const std::filesystem::path& executable_path);
    ~CustomPassGame();

    CustomPassGame(const CustomPassGame&) = delete;
    CustomPassGame& operator=(const CustomPassGame&) = delete;

    [[nodiscard]] const VulkanEngine::Application::ApplicationHooks& GetHooks() const { return hooks_; }

private:
    bool OnSetup(VulkanEngine::Application::ApplicationContext& ctx);
    // The Scene-stage module's body: the fallback-quad scene, the camera and the
    // three app passes. Runs inside InitRenderer, after the render graph exists.
    [[nodiscard]] bool BuildScene(VulkanEngine::Modules::ModuleContext& mctx);
    void OnFixedUpdate(const VulkanEngine::Application::ApplicationContext& ctx);
    void OnFrameUpdate(const VulkanEngine::Application::ApplicationContext& ctx);
    void OnFrameRender(const VulkanEngine::Application::ApplicationContext& ctx);
    void OnShutdown(VulkanEngine::Application::ApplicationContext& ctx);

    VulkanEngine::Application::ApplicationHooks hooks_{};
    VulkanShared::ScopedHandle<bool(VulkanEngine::Application::ApplicationContext&)> setup_token_{};
    VulkanShared::ScopedHandle<void(VulkanEngine::Application::ApplicationContext&)> fixed_update_token_{};
    VulkanShared::ScopedHandle<void(VulkanEngine::Application::ApplicationContext&)> frame_update_token_{};
    VulkanShared::ScopedHandle<void(VulkanEngine::Application::ApplicationContext&)> frame_render_token_{};
    VulkanShared::ScopedHandle<void(VulkanEngine::Application::ApplicationContext&)> shutdown_token_{};

    std::filesystem::path exe_dir_{};

    // App shader ids, registered in OnSetup and consumed by the Scene module
    // (which runs inside InitRenderer, after app shaders exist).
    VulkanEngine::ShaderSystem::ShaderId fullscreen_vert_{VulkanEngine::ShaderSystem::kInvalidShaderId};
    VulkanEngine::ShaderSystem::ShaderId capture_frag_{VulkanEngine::ShaderSystem::kInvalidShaderId};
    VulkanEngine::ShaderSystem::ShaderId tonemap_frag_{VulkanEngine::ShaderSystem::kInvalidShaderId};
    VulkanEngine::ShaderSystem::ShaderId exposure_comp_{VulkanEngine::ShaderSystem::kInvalidShaderId};

    VulkanEngine::RenderPipeline::RenderPipeline* pipeline_ = nullptr;
    VulkanEngine::RenderGraph::PassHandle capture_handle_{};
    VulkanEngine::RenderGraph::PassHandle exposure_handle_{};
    VulkanEngine::RenderGraph::PassHandle tonemap_handle_{};

    // All three app passes are toggled together: disabling only some would make
    // a later pass read a transient that was never written.
    bool passes_enabled_ = true;
    VulkanEngine::Input::ActionHandle toggle_passes_{};

    // Declared last: its module lambdas capture `this`, so the engine must be
    // destroyed before the state those lambdas read. ImGui is required because
    // ToneMapPass anchors itself to the engine's ImGui pass.
    VulkanEngine::GameEngine engine_game_{VulkanEngine::EngineFeatures{.imgui = true}};
};

} // namespace Examples::CustomPass::Game
