module;

#include <vector>
#include <string>
#include <filesystem>
#include <cstdint>

export module VulkanEngine.SceneLoader;

export import VulkanBackend.Runtime.VulkanBootstrap;
export import VulkanEngine.ResourceSystem;
export import VulkanEngine.ResourceSystem.TextureResource;
export import VulkanEngine.FileLoaders.Mesh.MeshMagicLoader;
export import VulkanEngine.GpuResources;
export import VulkanEngine.StandardMeshPipeline;
export import VulkanEngine.Mesh.MeshTypes;

export namespace VulkanEngine::SceneLoader {

struct MaterialDescriptor {
    std::filesystem::path texture_path;
    uint16_t technique_hint = 0;
};

struct LoadedMeshData {
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<float> uvs;
    std::vector<uint32_t> indices;
    std::vector<VulkanEngine::SubMesh> submeshes;
    std::vector<MaterialDescriptor> submesh_materials;
};

struct MeshInfo {
    std::string name;
    uint32_t vertex_offset = 0;
    uint32_t vertex_count = 0;
    uint32_t index_offset = 0;
    uint32_t index_count = 0;
    uint32_t first_submesh_index = 0;
    uint32_t submesh_count = 0;
};

struct CombinedScene {
    VulkanEngine::GpuResources::HeapAllocation vertex_allocation{};
    VulkanEngine::GpuResources::HeapAllocation index_allocation{};
    std::vector<MeshInfo> meshes{};
    std::vector<VulkanEngine::SubMesh> submeshes{};
};

class SceneLoader {
public:
    [[nodiscard]] static LoadedMeshData CreateFallbackQuad();

    [[nodiscard]] static LoadedMeshData LoadMeshFromFile(const std::filesystem::path& models_dir);

    [[nodiscard]] static LoadedMeshData LoadMeshFromFilePath(const std::filesystem::path& file_path);

    [[nodiscard]] static std::vector<VulkanEngine::StandardMeshPipeline::Vertex>
    ConvertToVertices(const LoadedMeshData& mesh);

    [[nodiscard]] static VulkanEngine::ResourceHandle<VulkanEngine::TextureResource>
    LoadTexture(VulkanEngine::ResourceManager& resource_manager,
                const std::filesystem::path& textures_dir,
                const VulkanEngine::ResourceHandle<VulkanEngine::TextureResource>& fallback);

    [[nodiscard]] static VulkanEngine::ResourceHandle<VulkanEngine::TextureResource>
    LoadTextureFromPath(VulkanEngine::ResourceManager& resource_manager,
                        const std::filesystem::path& texture_path,
                        const VulkanEngine::ResourceHandle<VulkanEngine::TextureResource>& fallback);

    [[nodiscard]] static bool LoadAllMeshes(const std::filesystem::path& models_dir,
                                             std::vector<LoadedMeshData>& out_meshes,
                                             std::vector<std::string>& out_names);

    [[nodiscard]] static CombinedScene UploadCombined(
        VulkanEngine::Runtime::VulkanBootstrap& bootstrap,
        VulkanEngine::GpuResources::StagingManager& staging_mgr,
        VulkanEngine::GpuResources::DeviceBufferHeap& vertex_heap,
        VulkanEngine::GpuResources::DeviceBufferHeap& index_heap,
        const std::vector<LoadedMeshData>& meshes);
};

}
