module;

#include <vector>
#include <filesystem>
#include <cstdint>
#include <memory>

export module App.DemoSceneRenderer;

export import VulkanEngine.Runtime.VulkanBootstrap;
export import VulkanEngine.ResourceSystem;
export import VulkanEngine.ResourceSystem.TextureResource;
export import VulkanEngine.FileLoaders.Mesh.BinMeshAssembler;
export import VulkanEngine.FileLoaders.Mesh.GltfMeshAssembler;
export import VulkanEngine.GpuResources;
export import VulkanEngine.MeshRendererSystem;
export import VulkanEngine.StandardMeshPipeline;

export namespace App::DemoSceneRenderer {

enum class RenderMode : uint8_t {
    Normal,
    Normals,
    NoTextures
};

struct MeshData {
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<float> uvs;
    std::vector<uint32_t> indices;
};

struct DemoVertex {
    float px = 0.0f;
    float py = 0.0f;
    float pz = 0.0f;
    float nx = 0.0f;
    float ny = 0.0f;
    float nz = 0.0f;
    float u = 0.0f;
    float v = 0.0f;
};

struct RenderPassData {
    VulkanEngine::Runtime::VulkanBootstrap* bootstrap = nullptr;
    uint32_t image_index = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

class DemoSceneManager {
public:
    [[nodiscard]] static MeshData CreateFallbackQuad();
    [[nodiscard]] static MeshData LoadMeshFromAssets(const std::filesystem::path& models_dir);

    [[nodiscard]] static VulkanEngine::ResourceHandle<VulkanEngine::TextureResource>
    LoadTexture(VulkanEngine::ResourceManager& resource_manager,
                const std::filesystem::path& textures_dir,
                const VulkanEngine::ResourceHandle<VulkanEngine::TextureResource>& fallback);

    [[nodiscard]] static std::vector<DemoVertex> ConvertToDemoVertices(const MeshData& mesh);

    static bool UploadDemoScene(VulkanEngine::Runtime::VulkanBootstrap& bootstrap,
                                const VulkanEngine::StandardMeshPipeline::Vertex* vertices,
                                uint32_t vertex_count,
                                const uint32_t* indices,
                                uint32_t index_count,
                                VulkanEngine::TextureResource* texture,
                                VulkanEngine::StandardMeshPipeline::PipelineManager* pipeline_manager);

    static void DestroyDemoSceneResources(VulkanEngine::Runtime::VulkanBootstrap& bootstrap);

    [[nodiscard]] static VulkanEngine::MeshRendererSystem::MeshRenderObject GetMeshRenderObject();

private:
    struct RawResources;
    static std::unique_ptr<RawResources> s_resources;

    static void DestroyRawResources(VulkanEngine::Runtime::IVulkanBootstrapBackend& backend);
};

} // namespace App::DemoSceneRenderer
