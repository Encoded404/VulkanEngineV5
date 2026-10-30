module;

export module Examples.BasicScene.Game;

import std;

export import VulkanEngine.GameEngine;
import VulkanShared.CallbackList;
import VulkanEngine.GplPolicy;
export import Examples.BasicScene.Components.SimpleControllerComponent;
export import Examples.BasicScene.Components.TransformControlComponent;

export namespace Examples::BasicScene::Game {

enum class RenderMode : std::uint8_t {
    Normal,
    Normals,
    NoTextures
};

class DemoGame {
public:
    DemoGame(RenderMode render_mode, const std::filesystem::path& executable_path,
             const std::filesystem::path model_path = {},
             const std::filesystem::path texture_path = {},
             VulkanEngine::ShaderSystem::GplPolicy gpl_policy = VulkanEngine::ShaderSystem::GplPolicy::Auto,
             VulkanEngine::ShaderSystem::GplStructurePolicy gpl_structure = VulkanEngine::ShaderSystem::GplStructurePolicy::Auto);
    ~DemoGame();

    DemoGame(const DemoGame&) = delete;
    DemoGame& operator=(const DemoGame&) = delete;

    [[nodiscard]] const VulkanEngine::Application::ApplicationHooks& GetHooks() const { return hooks_; }

private:
    bool OnSetup(VulkanEngine::Application::ApplicationContext& ctx);
    // The Scene-stage module's body: texture/material/meshes, camera, physical
    // camera and the ImGui debug panels. Runs inside InitRenderer, after the
    // renderer and ImGui system exist.
    [[nodiscard]] bool BuildScene(VulkanEngine::Modules::ModuleContext& mctx);
    void OnPreInput(VulkanEngine::Application::ApplicationContext& ctx);
    bool ShouldFilterMouseInput();
    bool ShouldFilterKeyboardInput();
    void OnFixedUpdate(const VulkanEngine::Application::ApplicationContext &ctx);
    void OnFrameUpdate(const VulkanEngine::Application::ApplicationContext &ctx);
    void OnFrameRender(const VulkanEngine::Application::ApplicationContext &ctx);
    void OnShutdown(VulkanEngine::Application::ApplicationContext& ctx);

    RenderMode render_mode_;
    VulkanEngine::Application::ApplicationHooks hooks_{};

    VulkanShared::ScopedHandle<bool(VulkanEngine::Application::ApplicationContext&)> setup_token_{};
    VulkanShared::ScopedHandle<void(VulkanEngine::Application::ApplicationContext&)> pre_input_token_{};
    VulkanShared::ScopedHandle<void(VulkanEngine::Application::ApplicationContext&)> fixed_update_token_{};
    VulkanShared::ScopedHandle<void(VulkanEngine::Application::ApplicationContext&)> frame_update_token_{};
    VulkanShared::ScopedHandle<void(VulkanEngine::Application::ApplicationContext&)> frame_render_token_{};
    VulkanShared::ScopedHandle<void(VulkanEngine::Application::ApplicationContext&)> shutdown_token_{};

    std::filesystem::path exe_dir_{};
    std::filesystem::path model_path_{};
    std::filesystem::path texture_path_{};

    VulkanEngine::ShaderSystem::GplPolicy gpl_policy_ = VulkanEngine::ShaderSystem::GplPolicy::Auto;
    VulkanEngine::ShaderSystem::GplStructurePolicy gpl_structure_ = VulkanEngine::ShaderSystem::GplStructurePolicy::Auto;

    // Object selector for the debug panel: name + control component.
    struct ControllableObject {
        std::string name{};
        Examples::BasicScene::Components::TransformControlComponent* component = nullptr;
    };
    std::vector<ControllableObject> controllable_objects_{};
    int selected_object_ = 0;

    // ImGui drives the debug panel; the physical camera is optional and both
    // its feature field and its use below are compile-time gated.
    VulkanShared::ScopedHandle<void()> imgui_draw_handle_{};

#ifdef VKENGINE_PHYSICAL_CAMERA
    VulkanShared::ScopedHandle<void()> imgui_camera_draw_handle_{};
    VulkanEngine::PhysicalCamera::PhysicalCameraHandle cam_handle_{};
    VulkanEngine::PhysicalCamera::PhysicalCameraTargetId cam_target_{};
    VulkanEngine::PhysicalCamera::PhysicalCameraBindingHandle cam_binding_{};
    std::uint32_t cam_target_slot_ = 0;
    std::uint32_t cam_native_slot_ = 0;
    std::uint32_t webcam_mesh_id_ = 0;
#endif

    // Declared last: its module lambdas capture `this`, so the engine must be
    // destroyed before the state those lambdas read.
#ifdef VKENGINE_PHYSICAL_CAMERA
    VulkanEngine::GameEngine engine_game_{
        VulkanEngine::EngineFeatures{.imgui = true, .physical_camera = true}};
#else
    VulkanEngine::GameEngine engine_game_{VulkanEngine::EngineFeatures{.imgui = true}};
#endif
};

} // namespace Examples::BasicScene::Game
