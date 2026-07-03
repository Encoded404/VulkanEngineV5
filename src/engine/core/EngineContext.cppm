module;

export module VulkanEngine.EngineContext;

import std;
import vulkan_hpp;

import VulkanEngine.ECS.ComponentRegistry;
import VulkanEngine.BindlessManager;
import VulkanEngine.SceneRenderer;
import VulkanEngine.TechniqueManager;
import VulkanEngine.Renderer;
import VulkanEngine.MeshManager;
import VulkanEngine.MeshRenderSystem;
import VulkanEngine.MeshRegistry;
import VulkanEngine.ResourceSystem;
import VulkanEngine.ResourceSystem.TextureResource;
import VulkanEngine.ImGui;
import VulkanBackend.ImGui;
import VulkanEngine.GpuResources;
import VulkanEngine.DefaultTextureFactory;
import VulkanEngine.StandardMeshPipeline;
import VulkanEngine.MaterialManager;

export namespace VulkanEngine::Game {

inline constexpr std::uint32_t FRAMES_IN_FLIGHT_DYN = 3;

struct GameConfig {
    StandardMeshPipeline::PipelineConfig pipeline_config{
        .depth_test_enable = true,
        .depth_write_enable = true,
        .depth_compare_op = vk::CompareOp::eLessOrEqual
    };
    Renderer::RendererConfig renderer_config{};
    std::uint64_t geometry_buffer_size_mb = 128;
    bool enable_imgui = true;
};

struct EngineContext {
    // GPU resources (constructed in order)
    GpuResources::StagingManager staging_mgr;
    GpuResources::DeviceBufferHeap vertex_heap;
    GpuResources::DeviceBufferHeap index_heap;
    std::array<GpuResources::DeviceBufferHeap, FRAMES_IN_FLIGHT_DYN> dynamic_vertex_heaps;
    std::array<GpuResources::DeviceBufferHeap, FRAMES_IN_FLIGHT_DYN> dynamic_index_heaps;

    // Rendering subsystems
    std::unique_ptr<BindlessManager::BindlessManager> bindless_mgr;
    std::unique_ptr<SceneRenderer::SceneRenderer> scene_renderer;
    std::unique_ptr<TechniqueManager::TechniqueManager> technique_mgr;
    std::unique_ptr<Renderer::Renderer> renderer;
    std::unique_ptr<MeshManager> mesh_manager;
    MeshRenderSystem mesh_render_system;
    MeshRegistry mesh_registry;
    VulkanEngine::ComponentRegistry component_registry{};

    // Assets
    ResourceManager resource_manager;
    std::shared_ptr<TextureResource> missing_texture;
    ResourceHandle<TextureResource> fallback_handle;

    // UI
    std::unique_ptr<ImGui::ImGuiSystem> imgui_system;
    std::shared_ptr<VulkanBackend::ImGui::IImGuiBackend> imgui_backend;

    // Material manager (no singleton — owned by context)
    VulkanEngine::MaterialManager::MaterialManager material_mgr{};

    // Convenience accessors
    auto& GetBindlessManager() { return *bindless_mgr; }
    auto& GetSceneRenderer() { return *scene_renderer; }
    auto& GetTechniqueManager() { return *technique_mgr; }
    auto& GetRenderer() { return *renderer; }
    auto& GetMeshManager() { return *mesh_manager; }
    auto& GetMeshRenderSystem() { return mesh_render_system; }
    auto& GetMeshRegistry() { return mesh_registry; }
    auto& GetComponentRegistry() { return component_registry; }
    auto& GetMaterialManager() { return material_mgr; }
    auto& GetResourceManager() { return resource_manager; }

    GpuResources::DeviceBufferHeap& GetVertexHeap() { return vertex_heap; }
    GpuResources::DeviceBufferHeap& GetIndexHeap() { return index_heap; }
    GpuResources::StagingManager& GetStagingManager() { return staging_mgr; }
    auto& GetDynamicVertexHeaps() { return dynamic_vertex_heaps; }
    auto& GetDynamicIndexHeaps() { return dynamic_index_heaps; }

    ImGui::ImGuiSystem* GetImGuiSystem() { return imgui_system.get(); }
    VulkanBackend::ImGui::IImGuiBackend* GetImGuiBackend() { return imgui_backend.get(); }
};

} // namespace VulkanEngine::Game
