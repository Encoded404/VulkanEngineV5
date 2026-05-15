module;

#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <algorithm>

#include <vulkan/vulkan_raii.hpp>

#include <logging/logging.hpp>

module App.DemoSceneRenderer;

import VulkanEngine.Components.MeshRenderer;
import VulkanEngine.Components.Transform;
import VulkanEngine.FileLoaders.Mesh.BinMeshAssembler;
import VulkanEngine.FileLoaders.Mesh.GltfMeshAssembler;
import VulkanEngine.Mesh.MeshTypes;
import VulkanEngine.GpuResources;
import VulkanEngine.MeshRendererSystem;
import VulkanEngine.StandardMeshPipeline;

namespace App::DemoSceneRenderer {

struct DemoSceneManager::RawResources {
    VulkanEngine::StandardMeshPipeline::MeshGPUResources gpu_resources{};
    VulkanEngine::StandardMeshPipeline::PipelineManager* pipeline_manager = nullptr;
    VulkanEngine::GpuResources::GpuDescriptorSet descriptor_set{};
};

std::unique_ptr<DemoSceneManager::RawResources> DemoSceneManager::s_resources = nullptr;

namespace {
    [[nodiscard]] std::filesystem::path FindFirstFileWithExtension(const std::filesystem::path& dir,
                                                                   const std::vector<std::string>& extensions) {
        if (!std::filesystem::exists(dir)) {
            LOGIFACE_LOG(debug, "Directory does not exist: " + dir.string());
            return {};
        }
        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            const std::string ext = entry.path().extension().string();
            if (std::ranges::find(extensions, ext) != extensions.end()) {
                return entry.path();
            }
        }
        return {};
    }

    [[nodiscard]] std::vector<std::byte> ReadBinaryFile(const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            throw std::runtime_error("Failed to open model file: " + path.string());
        }
        const std::streamsize size = file.tellg();
        if (size <= 0) {
            throw std::runtime_error("Model file is empty: " + path.string());
        }

        std::vector<std::byte> bytes(static_cast<size_t>(size));
        file.seekg(0, std::ios::beg);
        if (!file.read(reinterpret_cast<char*>(bytes.data()), size)) {
            throw std::runtime_error("Failed to read model file: " + path.string());
        }
        return bytes;
    }
}

MeshData DemoSceneManager::CreateFallbackQuad() {
    return MeshData{
        .positions = { -1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 0.0f, -1.0f, 1.0f, 0.0f },
        .normals = { 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f },
        .uvs = { 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f },
        .indices = { 0, 1, 2, 2, 3, 0 },
    };
}

MeshData DemoSceneManager::LoadMeshFromAssets(const std::filesystem::path& models_dir) {
    const auto bin_path = FindFirstFileWithExtension(models_dir, {".bin"});
    if (!bin_path.empty()) {
        try {
            const auto bytes = ReadBinaryFile(bin_path);
            auto assembler = std::make_shared<VulkanEngine::FileLoaders::Mesh::BinMeshAssembler>();
            auto mesh = assembler->AssembleFromFullBuffer(std::make_shared<std::vector<std::byte>>(bytes)).get();
            if (mesh) {
                MeshData data{};
                for (const auto& v : mesh->vertices) { data.positions.push_back(v.x); data.positions.push_back(v.y); data.positions.push_back(v.z); }
                for (const auto& n : mesh->normals) { data.normals.push_back(n.x); data.normals.push_back(n.y); data.normals.push_back(n.z); }
                for (const auto& uv : mesh->uvs) { data.uvs.push_back(uv.u); data.uvs.push_back(1.0f - uv.v); }
                data.indices = mesh->indices;
                return data;
            }
        } catch (const std::exception& e) {
            LOGIFACE_LOG(error, "got error in function LoadMeshFromAsset: " + std::string(e.what()));
        }
    }
    return CreateFallbackQuad();
}

VulkanEngine::ResourceHandle<VulkanEngine::TextureResource>
DemoSceneManager::LoadTexture(VulkanEngine::ResourceManager& resource_manager,
                              const std::filesystem::path& textures_dir,
                              const VulkanEngine::ResourceHandle<VulkanEngine::TextureResource>& fallback) {
    const auto texture_path = FindFirstFileWithExtension(textures_dir, {".ktx2", ".ktx", ".png", ".jpg", ".jpeg"});
    if (texture_path.empty()) return fallback;
    auto handle = resource_manager.LoadFromFile<VulkanEngine::TextureResource>(texture_path);
    return (handle.IsValid() && handle->IsLoaded()) ? handle : fallback;
}

std::vector<DemoVertex> DemoSceneManager::ConvertToDemoVertices(const MeshData& mesh) {
    std::vector<DemoVertex> vertices{};
    const size_t vertex_count = mesh.positions.size() / 3U;
    if (vertex_count == 0U) return vertices;
    vertices.resize(vertex_count);
    for (size_t i = 0; i < vertex_count; ++i) {
        const size_t p = i * 3U, t = i * 2U;
        vertices[i].px = mesh.positions[p+0]; vertices[i].py = mesh.positions[p+1]; vertices[i].pz = mesh.positions[p+2];
        if (mesh.normals.size() > p + 2) { vertices[i].nx = mesh.normals[p+0]; vertices[i].ny = mesh.normals[p+1]; vertices[i].nz = mesh.normals[p+2]; }
        vertices[i].u = (t + 1 < mesh.uvs.size()) ? mesh.uvs[t+0] : 0.0f;
        vertices[i].v = (t + 1 < mesh.uvs.size()) ? mesh.uvs[t+1] : 0.0f;
    }
    return vertices;
}

bool DemoSceneManager::UploadDemoScene(VulkanEngine::Runtime::VulkanBootstrap& bootstrap,
                                       const VulkanEngine::StandardMeshPipeline::Vertex* vertices,
                                       uint32_t vertex_count,
                                       const uint32_t* indices,
                                       uint32_t index_count,
                                       VulkanEngine::TextureResource* texture,
                                       VulkanEngine::StandardMeshPipeline::PipelineManager* pipeline_manager) {
    LOGIFACE_LOG(trace, "entering UploadDemoScene");
    auto& backend = bootstrap.GetBackend();
    DestroyRawResources(backend);
    s_resources = std::make_unique<RawResources>();

    auto cleanup_and_fail = [&]() {
        DestroyRawResources(backend);
        return false;
    };

    if (!pipeline_manager) {
        LOGIFACE_LOG(error, "PipelineManager is null");
        return cleanup_and_fail();
    }
    s_resources->pipeline_manager = pipeline_manager;

    s_resources->gpu_resources.vertex_buffer = VulkanEngine::GpuResources::GpuBuffer::Create(
        backend, vertex_count * sizeof(VulkanEngine::StandardMeshPipeline::Vertex),
        vk::BufferUsageFlagBits::eVertexBuffer,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
        vertices);

    s_resources->gpu_resources.index_buffer = VulkanEngine::GpuResources::GpuBuffer::Create(
        backend, index_count * sizeof(uint32_t),
        vk::BufferUsageFlagBits::eIndexBuffer,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
        indices);

    if (texture && texture->HasPixels()) {
        s_resources->gpu_resources.texture = VulkanEngine::GpuResources::GpuTexture::CreateFromPixels(
            backend,
            reinterpret_cast<const uint8_t*>(texture->GetPixels().data()),
            texture->GetWidth(),
            texture->GetHeight());
    }

    s_resources->descriptor_set = pipeline_manager->AllocateDescriptorSet(s_resources->gpu_resources.texture);

    s_resources->gpu_resources.index_count = index_count;
    LOGIFACE_LOG(trace, "leaving UploadDemoScene successfully");
    return true;
}

void DemoSceneManager::DestroyDemoSceneResources(VulkanEngine::Runtime::VulkanBootstrap& bootstrap) {
    DestroyRawResources(bootstrap.GetBackend());
}

VulkanEngine::MeshRendererSystem::MeshRenderObject DemoSceneManager::GetMeshRenderObject() {
    if (!s_resources || !s_resources->pipeline_manager) {
        return {};
    }

    VulkanEngine::MeshRendererSystem::MeshRenderObject obj{};
    obj.vertex_buffer = &s_resources->gpu_resources.vertex_buffer;
    obj.index_buffer = &s_resources->gpu_resources.index_buffer;
    obj.pipeline = s_resources->pipeline_manager->GetPipeline() ? &*(*s_resources->pipeline_manager->GetPipeline()) : nullptr;
    obj.pipeline_layout = s_resources->pipeline_manager->GetPipelineLayout();
    obj.descriptor_set = s_resources->descriptor_set.IsValid() ? s_resources->descriptor_set.GetHandle() : nullptr;
    obj.index_count = s_resources->gpu_resources.index_count;

    return obj;
}

void DemoSceneManager::DestroyRawResources(VulkanEngine::Runtime::IVulkanBootstrapBackend& backend) {
    if (s_resources) {
        backend.GetDevice().waitIdle();
        s_resources.reset();
    }
}

} // namespace App::DemoSceneRenderer
