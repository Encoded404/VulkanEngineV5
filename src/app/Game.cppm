module;

#include <cstdint>
#include <memory>
#include <string>
#include <filesystem>

export module App.Game;

export import VulkanEngine.Application;
export import VulkanEngine.DefaultRenderer;
export import VulkanEngine.SceneLoader;
export import VulkanEngine.TechniqueManager;
export import VulkanEngine.MaterialManager;
export import VulkanEngine.BindlessManager;
export import VulkanEngine.SceneRenderer;
export import VulkanEngine.Components.Camera;
export import VulkanEngine.Components.Transform;
export import VulkanEngine.Components.MeshReference;
export import VulkanEngine.Components.Material;
export import VulkanEngine.StandardMeshPipeline;
export import VulkanEngine.ImGuiSystem;
export import VulkanEngine.ResourceSystem;
export import VulkanEngine.ResourceSystem.TextureResource;
export import VulkanEngine.DefaultResources;
export import VulkanEngine.ShaderLoader;
export import VulkanEngine.Input;
export import VulkanEngine.GpuResources;
export import App.Components.DemoInputComponent;

export namespace App::Game {

enum class RenderMode : uint8_t {
    Normal,
    Normals,
    NoTextures
};

class DemoGame {
public:
    DemoGame(const std::string& log_level, RenderMode render_mode, const std::filesystem::path& executable_path);
    ~DemoGame();

    DemoGame(const DemoGame&) = delete;
    DemoGame& operator=(const DemoGame&) = delete;

    [[nodiscard]] const VulkanEngine::Application::ApplicationHooks& GetHooks() const { return hooks_; }

private:
    bool OnSetup(VulkanEngine::Application::ApplicationContext& ctx);
    void OnPreInput(VulkanEngine::Application::ApplicationContext& ctx);
    bool ShouldFilterMouseInput();
    bool ShouldFilterKeyboardInput();
    void OnFrameUpdate(VulkanEngine::Application::ApplicationContext& ctx);
    void OnFrameRender(VulkanEngine::Application::ApplicationContext& ctx);
    void OnShutdown(VulkanEngine::Application::ApplicationContext& ctx);

    RenderMode render_mode_;
    VulkanEngine::Application::ApplicationHooks hooks_{};

    std::filesystem::path exe_dir_{};
    VulkanEngine::ResourceManager resource_manager_{};
    std::shared_ptr<VulkanEngine::TextureResource> missing_texture_{};
    VulkanEngine::ResourceHandle<VulkanEngine::TextureResource> fallback_handle_{};
    VulkanEngine::Components::Camera* camera_ = nullptr;
    std::unique_ptr<VulkanEngine::DefaultRenderer::DefaultRenderer> renderer_{};
    std::unique_ptr<VulkanEngine::SceneRenderer::SceneRenderer> scene_renderer_{};
    std::unique_ptr<VulkanEngine::TechniqueManager::TechniqueManager> technique_mgr_{};
    std::unique_ptr<VulkanEngine::MaterialManager::MaterialManager> material_mgr_{};
    std::unique_ptr<VulkanEngine::BindlessManager::BindlessManager> bindless_mgr_{};
    std::shared_ptr<VulkanEngine::Backend::ImGui::IImGuiBackend> imgui_backend_{};
    std::unique_ptr<VulkanEngine::ImGuiSystem::ImGuiSystem> imgui_system_{};
    VulkanEngine::SceneLoader::CombinedScene combined_scene_{};
    bool scene_valid_ = false;
};

} // namespace App::Game
