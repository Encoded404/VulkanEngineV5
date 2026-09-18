module;

export module Examples.CustomPass.Game;

import std;

export import VulkanEngine.GameEngine;
import VulkanShared.CallbackList;

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
    void OnFrameUpdate(const VulkanEngine::Application::ApplicationContext& ctx);
    void OnFrameRender(const VulkanEngine::Application::ApplicationContext& ctx);
    void OnShutdown(VulkanEngine::Application::ApplicationContext& ctx);

    VulkanEngine::Application::ApplicationHooks hooks_{};
    VulkanShared::ScopedHandle<bool(VulkanEngine::Application::ApplicationContext&)> setup_token_{};
    VulkanShared::ScopedHandle<void(VulkanEngine::Application::ApplicationContext&)> frame_update_token_{};
    VulkanShared::ScopedHandle<void(VulkanEngine::Application::ApplicationContext&)> frame_render_token_{};
    VulkanShared::ScopedHandle<void(VulkanEngine::Application::ApplicationContext&)> shutdown_token_{};

    std::filesystem::path exe_dir_{};
    VulkanEngine::GameEngine engine_game_{};

    VulkanEngine::RenderPipeline::RenderPipeline* pipeline_ = nullptr;
    VulkanEngine::RenderGraph::PassHandle capture_handle_{};
    VulkanEngine::RenderGraph::PassHandle exposure_handle_{};
    VulkanEngine::RenderGraph::PassHandle tonemap_handle_{};

    // All three app passes are toggled together: disabling only some would make
    // a later pass read a transient that was never written.
    bool passes_enabled_ = true;
    VulkanEngine::Input::ActionHandle toggle_passes_{};
};

} // namespace Examples::CustomPass::Game
