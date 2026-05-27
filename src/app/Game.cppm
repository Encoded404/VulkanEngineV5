module;

#include <memory>
#include <filesystem>

export module App.Game;

export import VulkanEngine.Game;
export import App.Components.DemoInputComponent;

export namespace App::Game {

enum class RenderMode : uint8_t {
    Normal,
    Normals,
    NoTextures
};

class DemoGame {
public:
    DemoGame(RenderMode render_mode, const std::filesystem::path& executable_path,
             const std::filesystem::path model_path = {},
             const std::filesystem::path texture_path = {});
    ~DemoGame();

    DemoGame(const DemoGame&) = delete;
    DemoGame& operator=(const DemoGame&) = delete;

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

    std::filesystem::path exe_dir_{};
    std::filesystem::path model_path_{};
    std::filesystem::path texture_path_{};

    VulkanEngine::Game::GameEngine engine_game_{};
};

} // namespace App::Game
