module;

#include <vector>
#include <filesystem>
#include <cstdint>
#include <memory>
#include <vulkan/vulkan_raii.hpp>

export module App.DemoSceneRenderer;

import VulkanEngine.Runtime.VulkanBootstrap;
import VulkanEngine.RenderGraph;
import VulkanEngine.RenderGraph.GraphExecutionContext;
import VulkanEngine.ResourceSystem;
import VulkanEngine.ResourceSystem.TextureResource;
import VulkanEngine.FileLoaders.Mesh.BinMeshAssembler;
import VulkanEngine.FileLoaders.Mesh.GltfMeshAssembler;

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

struct FrameRenderData {
    VulkanEngine::Runtime::VulkanBootstrap* bootstrap = nullptr;
    const VulkanEngine::RenderGraph::GraphExecutionContext* graph_context = nullptr;
    float angle_degrees = 0.0f;
    uint32_t image_index = 0; // Added image_index
    bool render_success = true;
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
    [[nodiscard]] static std::vector<uint32_t> ReadSpirv(const std::filesystem::path& path);

    static bool UploadDemoScene(VulkanEngine::Runtime::VulkanBootstrap& bootstrap,
                                const DemoVertex* vertices,
                                uint32_t vertex_count,
                                const uint32_t* indices,
                                uint32_t index_count,
                                VulkanEngine::TextureResource* texture,
                                const uint32_t* vert_spv,
                                size_t vert_spv_bytes,
                                const uint32_t* frag_spv,
                                size_t frag_spv_bytes);

    static void DestroyDemoSceneResources(VulkanEngine::Runtime::VulkanBootstrap& bootstrap);

    static bool RenderDemoFrame(VulkanEngine::Runtime::VulkanBootstrap& bootstrap,
                                uint32_t image_index,
                                float angle_degrees);

private:
    struct RawResources;
    static std::unique_ptr<RawResources> s_resources;

    static void DestroyRawResources(VulkanEngine::Runtime::IVulkanBootstrapBackend& backend);
    static bool CreateBuffer(VulkanEngine::Runtime::IVulkanBootstrapBackend& backend, uint64_t size, vk::BufferUsageFlags usage, vk::MemoryPropertyFlags properties,
                             std::unique_ptr<vk::raii::Buffer>& out_buffer, std::unique_ptr<vk::raii::DeviceMemory>& out_memory);
    static bool CreateTexture(VulkanEngine::Runtime::IVulkanBootstrapBackend& backend, const uint8_t* pixels, uint32_t width, uint32_t height);
    static bool CreatePipeline(VulkanEngine::Runtime::IVulkanBootstrapBackend& backend, const uint32_t* vert_spv, size_t vert_size, const uint32_t* frag_spv, size_t frag_size, uint32_t vertex_stride);
};

} // namespace App::DemoSceneRenderer
