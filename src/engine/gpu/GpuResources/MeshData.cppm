module;

#include <cstdint>
#include <vector>

export module VulkanEngine.GpuResources.MeshData;

export import VulkanEngine.StandardMeshPipeline;
export import VulkanEngine.Mesh.MeshTypes;

export namespace VulkanEngine::GpuResources {

struct MeshData {
    std::vector<StandardMeshPipeline::Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<SubMesh> sub_meshes;
};

class IMeshSource {
public:
    virtual ~IMeshSource() = default;
    virtual MeshData Load() = 0;
};

} // namespace VulkanEngine::GpuResources
