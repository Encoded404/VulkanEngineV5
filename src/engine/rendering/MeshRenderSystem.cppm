module;

#include <cstdint>
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_RADIANS
#include <glm/glm.hpp> // NOLINT(misc-include-cleaner)

export module VulkanEngine.MeshRenderSystem;

export import VulkanBackend.Component;
export import VulkanEngine.Components.Transform;
export import VulkanEngine.Components.MeshReference;
export import VulkanEngine.Components.DynamicMesh;
export import VulkanEngine.MeshRegistry;
export import VulkanEngine.MeshManager;
export import VulkanEngine.SceneRenderer;
export import VulkanEngine.GpuResources.DeviceBufferHeap;
export import VulkanEngine.Mesh.MeshTypes;
export import VulkanEngine.MaterialManager;
export import VulkanEngine.BindlessManager;

export namespace VulkanEngine {

class MeshRenderSystem {
public:
    MeshRenderSystem() = default;

    MeshRenderSystem(const MeshRenderSystem&) = delete;
    MeshRenderSystem& operator=(const MeshRenderSystem&) = delete;

    static constexpr uint32_t EVICTION_TIMEOUT_FRAMES = 120;
    static constexpr uint32_t MAX_GATHER = 65536;

    void ProcessFrame(ComponentRegistry& registry,
                      MeshRegistry& mesh_registry,
                      MeshManager& mesh_mgr,
                      SceneRenderer::SceneRenderer& renderer,
                      GpuResources::DeviceBufferHeap& vtx_heap,
                      GpuResources::DeviceBufferHeap& idx_heap,
                      uint32_t frame_index);

private:
    struct DrawEntity {
        const Components::Transform* transform = nullptr;
        const Components::MeshReference* mesh_ref = nullptr;
    };

    struct DynamicEntity {
        const Components::Transform* transform = nullptr;
        const Components::DynamicMesh* dyn_mesh = nullptr;
    };
};

}

