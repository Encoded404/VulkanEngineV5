module;

#include <filesystem>

export module App.Game;

export import VulkanEngine.Game;
import VulkanBackend.Utils.CallbackList;
export import App.Components.SimpleControllerComponent;
export import App.Components.TransformControlComponent;

export namespace App::Game {

enum class RenderMode : uint8_t {
    Normal,
    Normals,
    NoTextures
};

class GraphGame {
public:
    GraphGame(RenderMode render_mode, const std::filesystem::path& executable_path,
             const std::filesystem::path model_path = {},
             const std::filesystem::path texture_path = {});
    ~GraphGame();

    GraphGame(const GraphGame&) = delete;
    GraphGame& operator=(const GraphGame&) = delete;

    [[nodiscard]] const VulkanEngine::Application::ApplicationHooks& GetHooks() const { return hooks_; }

private:
    bool OnSetup(VulkanEngine::Application::ApplicationContext& ctx);
    void OnPreInput(VulkanEngine::Application::ApplicationContext& ctx);
    bool ShouldFilterMouseInput();
    bool ShouldFilterKeyboardInput();
    void OnFrameUpdate(const VulkanEngine::Application::ApplicationContext &ctx);
    void OnFrameRender(const VulkanEngine::Application::ApplicationContext &ctx);
    void OnShutdown(VulkanEngine::Application::ApplicationContext& ctx);

    RenderMode render_mode_;
    VulkanEngine::Application::ApplicationHooks hooks_{};

    VulkanEngine::Utils::ScopedHandle<bool(VulkanEngine::Application::ApplicationContext&)> setup_token_{};
    VulkanEngine::Utils::ScopedHandle<void(VulkanEngine::Application::ApplicationContext&)> pre_input_token_{};
    VulkanEngine::Utils::ScopedHandle<void(VulkanEngine::Application::ApplicationContext&)> frame_update_token_{};
    VulkanEngine::Utils::ScopedHandle<void(VulkanEngine::Application::ApplicationContext&)> frame_render_token_{};
    VulkanEngine::Utils::ScopedHandle<void(VulkanEngine::Application::ApplicationContext&)> shutdown_token_{};

    std::filesystem::path exe_dir_{};
    std::filesystem::path model_path_{};
    std::filesystem::path texture_path_{};

    VulkanEngine::Game::GameEngine engine_game_{};
    VulkanEngine::Utils::ScopedHandle<void()> imgui_draw_handle_{};
};

} // namespace App::Game
