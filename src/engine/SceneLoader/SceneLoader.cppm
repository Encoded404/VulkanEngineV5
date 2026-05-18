module;

#include <vector>
#include <string>
#include <filesystem>
#include <cstdint>

export module VulkanEngine.SceneLoader;

export import VulkanBackend.Runtime.VulkanBootstrap;
export import VulkanEngine.ResourceSystem;
export import VulkanEngine.ResourceSystem.TextureResource;
export import VulkanEngine.FileLoaders.Mesh.BinMeshAssembler;
export import VulkanEngine.FileLoaders.Mesh.GltfMeshAssembler;
export import VulkanEngine.GpuResources;
export import VulkanEngine.StandardMeshPipeline;

export namespace VulkanEngine::SceneLoader {

struct LoadedMeshData {
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<float> uvs;
    std::vector<uint32_t> indices;
};

struct MeshInfo {
    std::string name;
    uint32_t vertex_offset = 0;    // in vertices
    uint32_t index_offset = 0;     // in indices
    uint32_t index_count = 0;
};

struct CombinedScene {
    VulkanEngine::GpuResources::GpuBuffer vertex_buffer{};
    VulkanEngine::GpuResources::GpuBuffer index_buffer{};
    std::vector<MeshInfo> meshes{};
};

class SceneManager {
public:
    [[nodiscard]] static LoadedMeshData CreateFallbackQuad();

    [[nodiscard]] static LoadedMeshData LoadMeshFromFile(const std::filesystem::path& models_dir);

    [[nodiscard]] static std::vector<VulkanEngine::StandardMeshPipeline::Vertex>
    ConvertToVertices(const LoadedMeshData& mesh);

    [[nodiscard]] static std::vector<VulkanEngine::StandardMeshPipeline::Vertex>
    ConvertToVertices(const LoadedMeshData& mesh,
                      uint16_t default_material_id);

    [[nodiscard]] static VulkanEngine::ResourceHandle<VulkanEngine::TextureResource>
    LoadTexture(VulkanEngine::ResourceManager& resource_manager,
                const std::filesystem::path& textures_dir,
                const VulkanEngine::ResourceHandle<VulkanEngine::TextureResource>& fallback);

    [[nodiscard]] static bool LoadAllMeshes(const std::filesystem::path& models_dir,
                                             std::vector<LoadedMeshData>& out_meshes,
                                             std::vector<std::string>& out_names);

    [[nodiscard]] static CombinedScene UploadCombined(
        VulkanEngine::Runtime::VulkanBootstrap& bootstrap,
        const std::vector<LoadedMeshData>& meshes,
        const std::vector<uint16_t>& material_ids_for_meshes,
        uint16_t default_material_id);
};

}
