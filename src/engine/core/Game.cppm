module;

#include <memory>
#include <filesystem>
#include <vector>
#include <cstdint>
#include <string>

export module VulkanEngine.Game;

export import VulkanEngine.Renderer;
export import VulkanEngine.SceneLoader;
export import VulkanEngine.TechniqueManager;
export import VulkanEngine.MaterialManager;
export import VulkanEngine.BindlessManager;
export import VulkanEngine.SceneRenderer;
export import VulkanEngine.Components.Camera;
export import VulkanEngine.Components.Transform;
export import VulkanEngine.Components.MeshReference;
export import VulkanEngine.StandardMeshPipeline;
export import VulkanEngine.ImGui;
export import VulkanEngine.ResourceSystem;
export import VulkanEngine.ResourceSystem.TextureResource;
export import VulkanEngine.DefaultTextureFactory;
export import VulkanEngine.ShaderLoader;
export import VulkanEngine.GpuResources;
export import VulkanEngine.Application;
export import VulkanEngine.Input;
export import VulkanBackend.ImGui;
import VulkanBackend.Platform.SdlPlatform;
import VulkanBackend.Utils.CallbackList;

export namespace VulkanEngine::Game {

struct GameConfig {
    std::filesystem::path shader_dir;
    std::string vertex_shader_file = "main_world.vert.spv";
    std::string fragment_shader_file = "standard_mesh.frag.spv";
    StandardMeshPipeline::PipelineConfig pipeline_config{
        .depth_test_enable = true,
        .depth_write_enable = true,
        .depth_compare_op = vk::CompareOp::eLessOrEqual
    };
    Renderer::RendererConfig renderer_config{};
    uint64_t geometry_buffer_size_mb = 128;
    bool enable_imgui = true;
};

class GameEngine {
public:
    GameEngine() = default;
    ~GameEngine();
    GameEngine(const GameEngine&) = delete;
    GameEngine& operator=(const GameEngine&) = delete;

    bool Setup(VulkanEngine::Application::ApplicationContext& ctx, const GameConfig& config);

    uint32_t LoadTexture(VulkanEngine::Application::ApplicationContext& ctx, const std::filesystem::path& path);

    bool InitRenderer(VulkanEngine::Application::ApplicationContext& ctx);

    const SceneLoader::CombinedScene& UploadScene(
        VulkanEngine::Application::ApplicationContext& ctx,
        const std::vector<SceneLoader::LoadedMeshData>& meshes);

    Components::Camera& CreateCamera(ComponentRegistry& registry);
    Components::Camera* GetCamera() { return camera_; }

    void FrameUpdate(const VulkanEngine::Application::ApplicationContext& ctx);
    void FrameRender(const VulkanEngine::Application::ApplicationContext& ctx);
    void Shutdown();

    BindlessManager::BindlessManager& GetBindlessManager() { return *bindless_mgr_; }
    SceneRenderer::SceneRenderer& GetSceneRenderer() { return *scene_renderer_; }
    TechniqueManager::TechniqueManager& GetTechniqueManager() { return *technique_mgr_; }
    Renderer::Renderer& GetRenderer() { return *renderer_; }
    ImGui::ImGuiSystem* GetImGuiSystem() { return imgui_system_.get(); }
    GpuResources::DeviceBufferHeap& GetVertexHeap() { return vertex_heap_; }
    GpuResources::DeviceBufferHeap& GetIndexHeap() { return index_heap_; }
    GpuResources::StagingManager& GetStagingManager() { return staging_mgr_; }
    const SceneLoader::CombinedScene& GetCombinedScene() { return combined_scene_; }
    uint16_t GetMainTechniqueId() const { return main_technique_id_; }
    bool IsInitialized() const { return initialized_; }

private:
    uint32_t UploadTextureToBindless(VulkanEngine::Application::ApplicationContext& ctx, TextureResource* tex);

    std::unique_ptr<BindlessManager::BindlessManager> bindless_mgr_;
    std::unique_ptr<SceneRenderer::SceneRenderer> scene_renderer_;
    std::unique_ptr<TechniqueManager::TechniqueManager> technique_mgr_;
    std::unique_ptr<Renderer::Renderer> renderer_;
    std::unique_ptr<ImGui::ImGuiSystem> imgui_system_;
    std::shared_ptr<Backend::ImGui::IImGuiBackend> imgui_backend_;

    GpuResources::DeviceBufferHeap vertex_heap_;
    GpuResources::DeviceBufferHeap index_heap_;
    GpuResources::StagingManager staging_mgr_;

    SceneLoader::CombinedScene combined_scene_;
    bool scene_valid_ = false;

    ResourceManager resource_manager_;
    std::shared_ptr<TextureResource> missing_texture_;
    ResourceHandle<TextureResource> fallback_handle_;

    Components::Camera* camera_ = nullptr;

    uint16_t main_technique_id_ = 0;
    bool initialized_ = false;

    std::vector<uint32_t> vert_spv_holder_;
    std::vector<uint32_t> frag_spv_holder_;

    VulkanEngine::Utils::ScopedHandle<void(void*)> imgui_event_token_{};

    VulkanEngine::Runtime::VulkanBootstrap* bootstrap_ = nullptr;
    GameConfig config_{};
};

} // namespace VulkanEngine::Game

