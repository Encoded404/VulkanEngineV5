module;

export module VulkanEngine.Game;

import std;

import vulkan_hpp;

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
export import VulkanEngine.EngineContext;

import VulkanBackend.Platform.SdlPlatform;
import VulkanShared.CallbackList;
import VulkanEngine.MeshManager;
import VulkanEngine.MeshRegistry;
import VulkanEngine.MeshRenderSystem;
import VulkanEngine.EngineBootstrap;

export namespace VulkanEngine::Game {

class GameEngine {
public:
    GameEngine() = default;
    ~GameEngine();
    GameEngine(const GameEngine&) = delete;
    GameEngine& operator=(const GameEngine&) = delete;

    bool Setup(VulkanEngine::Application::ApplicationContext& ctx, const GameConfig& config);

    std::uint32_t LoadTexture(VulkanEngine::Application::ApplicationContext& ctx, const std::filesystem::path& path);

    bool InitRenderer(VulkanEngine::Application::ApplicationContext& ctx,
                      std::span<const std::uint32_t> vert_override = {},
                      std::span<const std::uint32_t> frag_override = {});

    struct UploadedMesh {
        std::uint32_t first_submesh = 0;
        std::uint32_t submesh_count = 0;
        std::uint8_t vertex_buffer_index = 0;
        std::uint8_t index_buffer_index = 0;
    };

    std::vector<UploadedMesh> UploadScene(
        VulkanEngine::Application::ApplicationContext& ctx,
        const std::vector<VulkanEngine::GpuResources::MeshData>& meshes);
    std::vector<UploadedMesh> UploadSceneFromFiles(
        VulkanEngine::Application::ApplicationContext& ctx,
        const std::vector<std::filesystem::path>& file_paths,
        const std::vector<SceneLoader::MaterialId>* material_bindings = nullptr);

    Components::Camera& CreateCamera(ComponentRegistry& registry);
    Components::Camera* GetCamera() { return camera_; }

    void FrameUpdate(const VulkanEngine::Application::ApplicationContext& ctx);
    void FrameRender(const VulkanEngine::Application::ApplicationContext& ctx);
    void Shutdown();

    // Subsystem accessors delegate to EngineContext
    ResourceManager& GetResourceManager() { return ctx_.resource_manager; }
    BindlessManager::BindlessManager& GetBindlessManager() { return *ctx_.bindless_mgr; }
    SceneRenderer::SceneRenderer& GetSceneRenderer() { return *ctx_.scene_renderer; }
    TechniqueManager::TechniqueManager& GetTechniqueManager() { return *ctx_.technique_mgr; }
    Renderer::Renderer& GetRenderer() { return *ctx_.renderer; }
    ImGui::ImGuiSystem* GetImGuiSystem() { return ctx_.imgui_system.get(); }
    GpuResources::DeviceBufferHeap& GetVertexHeap() { return ctx_.vertex_heap; }
    GpuResources::DeviceBufferHeap& GetIndexHeap() { return ctx_.index_heap; }
    GpuResources::StagingManager& GetStagingManager() { return ctx_.staging_mgr; }
    std::array<GpuResources::DeviceBufferHeap, FRAMES_IN_FLIGHT_DYN>& GetDynamicVertexHeaps() { return ctx_.dynamic_vertex_heaps; }
    std::array<GpuResources::DeviceBufferHeap, FRAMES_IN_FLIGHT_DYN>& GetDynamicIndexHeaps() { return ctx_.dynamic_index_heaps; }
    MeshManager& GetMeshManager() { return *ctx_.mesh_manager; }
    MeshRegistry& GetMeshRegistry() { return ctx_.mesh_registry; }
    MeshRenderSystem& GetMeshRenderSystem() { return ctx_.mesh_render_system; }
    std::uint16_t GetMainTechniqueIdRaw() const { return main_technique_id_; }
    VulkanEngine::TechniqueManager::TechniqueId GetMainTechniqueId() const { return VulkanEngine::TechniqueManager::TechniqueId{main_technique_id_}; }
    bool IsInitialized() const { return initialized_; }
    void MarkSceneValid() { scene_valid_ = true; }
    std::uint32_t UploadTextureToBindless(VulkanEngine::Application::ApplicationContext& ctx, TextureResource* tex);

    EngineContext& GetContext() { return ctx_; }
    const EngineContext& GetContext() const { return ctx_; }

private:
    EngineContext ctx_;
    EngineBootstrap bootstrap_;

    VulkanShared::ScopedHandle<void(void*)> imgui_event_token_{};

    VulkanBackend::Runtime::VulkanBootstrap* vk_backend_ = nullptr;
    GameConfig config_{};

    Components::Camera* camera_ = nullptr;

    std::uint16_t main_technique_id_ = 0;
    bool scene_valid_ = false;
    bool initialized_ = false;

    std::vector<std::uint32_t> vert_spv_holder_;
    std::vector<std::uint32_t> frag_spv_holder_;
};

} // namespace VulkanEngine::Game
