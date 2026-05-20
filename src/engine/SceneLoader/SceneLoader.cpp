module;

#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <algorithm>

#include <vulkan/vulkan_raii.hpp>

#include <logging/logging.hpp>

module VulkanEngine.SceneLoader;

import VulkanEngine.Components.Transform;
import VulkanEngine.FileLoaders.Mesh.BinMeshAssembler;
import VulkanEngine.FileLoaders.Mesh.GltfMeshAssembler;
import VulkanEngine.Mesh.MeshTypes;
import VulkanEngine.GpuResources;
import VulkanEngine.StandardMeshPipeline;

namespace VulkanEngine::SceneLoader {

namespace {
    [[nodiscard]] std::filesystem::path FindFirstFileWithExtension(const std::filesystem::path& dir,
                                                                    const std::vector<std::string>& extensions) {
        if (!std::filesystem::exists(dir)) {
            LOGIFACE_LOG(debug, "Directory does not exist: " + dir.string());
            return {};
        }
        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            if (!entry.is_regular_file()) continue;
            const std::string ext = entry.path().extension().string();
            if (std::ranges::find(extensions, ext) != extensions.end()) {
                return entry.path();
            }
        }
        return {};
    }

    [[nodiscard]] std::vector<std::filesystem::path> FindAllFilesWithExtensions(
        const std::filesystem::path& dir, const std::vector<std::string>& extensions) {
        std::vector<std::filesystem::path> result;
        if (!std::filesystem::exists(dir)) return result;
        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            if (!entry.is_regular_file()) continue;
            const std::string ext = entry.path().extension().string();
            if (std::ranges::find(extensions, ext) != extensions.end()) {
                result.push_back(entry.path());
            }
        }
        return result;
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

LoadedMeshData SceneManager::CreateFallbackQuad() {
    return LoadedMeshData{
        .positions = { -1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 0.0f, -1.0f, 1.0f, 0.0f },
        .normals = { 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f },
        .uvs = { 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f },
        .indices = { 0, 1, 2, 2, 3, 0 },
    };
}

LoadedMeshData SceneManager::LoadMeshFromFile(const std::filesystem::path& models_dir) {
    const auto bin_path = FindFirstFileWithExtension(models_dir, {".bin"});
    if (!bin_path.empty()) {
        try {
            const auto bytes = ReadBinaryFile(bin_path);
            auto assembler = std::make_shared<VulkanEngine::FileLoaders::Mesh::BinMeshAssembler>();
            auto mesh = assembler->AssembleFromFullBuffer(std::make_shared<std::vector<std::byte>>(bytes)).get();
            if (mesh) {
                LoadedMeshData data{};
                for (const auto& v : mesh->vertices) { data.positions.push_back(v.x); data.positions.push_back(v.y); data.positions.push_back(v.z); }
                for (const auto& n : mesh->normals) { data.normals.push_back(n.x); data.normals.push_back(n.y); data.normals.push_back(n.z); }
                for (const auto& uv : mesh->uvs) { data.uvs.push_back(uv.u); data.uvs.push_back(1.0f - uv.v); }
                data.indices = mesh->indices;
                return data;
            }
        } catch (const std::exception& e) {
            LOGIFACE_LOG(error, "got error in function LoadMeshFromFile: " + std::string(e.what()));
        }
    }
    return CreateFallbackQuad();
}

bool SceneManager::LoadAllMeshes(const std::filesystem::path& models_dir,
                                  std::vector<LoadedMeshData>& out_meshes,
                                  std::vector<std::string>& out_names) {
    const auto bin_files = FindAllFilesWithExtensions(models_dir, {".bin"});
    if (bin_files.empty()) {
        LOGIFACE_LOG(warn, "No .bin files found in: " + models_dir.string());
        return false;
    }

    for (const auto& path : bin_files) {
        try {
            const auto bytes = ReadBinaryFile(path);
            auto assembler = std::make_shared<VulkanEngine::FileLoaders::Mesh::BinMeshAssembler>();
            auto mesh = assembler->AssembleFromFullBuffer(std::make_shared<std::vector<std::byte>>(bytes)).get();
            if (mesh) {
                LoadedMeshData data{};
                for (const auto& v : mesh->vertices) { data.positions.push_back(v.x); data.positions.push_back(v.y); data.positions.push_back(v.z); }
                for (const auto& n : mesh->normals) { data.normals.push_back(n.x); data.normals.push_back(n.y); data.normals.push_back(n.z); }
                for (const auto& uv : mesh->uvs) { data.uvs.push_back(uv.u); data.uvs.push_back(1.0f - uv.v); }
                data.indices = mesh->indices;
                out_meshes.push_back(std::move(data));
                out_names.push_back(path.stem().string());
            }
        } catch (const std::exception& e) {
            LOGIFACE_LOG(error, "Failed to load mesh: " + path.string() + " - " + std::string(e.what()));
        }
    }
    return !out_meshes.empty();
}

VulkanEngine::ResourceHandle<VulkanEngine::TextureResource>
SceneManager::LoadTexture(VulkanEngine::ResourceManager& resource_manager,
                          const std::filesystem::path& textures_dir,
                          const VulkanEngine::ResourceHandle<VulkanEngine::TextureResource>& fallback) {
    const auto texture_path = FindFirstFileWithExtension(textures_dir, {".ktx2", ".ktx", ".png", ".jpg", ".jpeg"});
    if (texture_path.empty()) return fallback;
    auto handle = resource_manager.LoadFromFile<VulkanEngine::TextureResource>(texture_path);
    return (handle.IsValid() && handle->IsLoaded()) ? handle : fallback;
}

std::vector<VulkanEngine::StandardMeshPipeline::Vertex>
SceneManager::ConvertToVertices(const LoadedMeshData& mesh) {
    return ConvertToVertices(mesh, 0);
}

std::vector<VulkanEngine::StandardMeshPipeline::Vertex>
SceneManager::ConvertToVertices(const LoadedMeshData& mesh, uint16_t default_material_id) {
    std::vector<VulkanEngine::StandardMeshPipeline::Vertex> vertices{};
    const size_t vertex_count = mesh.positions.size() / 3U;
    if (vertex_count == 0U) return vertices;
    vertices.resize(vertex_count);
    for (size_t i = 0; i < vertex_count; ++i) {
        const size_t p = i * 3U, t = i * 2U;
        vertices[i].px = mesh.positions[p + 0];
        vertices[i].py = mesh.positions[p + 1];
        vertices[i].pz = mesh.positions[p + 2];
        if (mesh.normals.size() > p + 2) {
            vertices[i].nx = mesh.normals[p + 0];
            vertices[i].ny = mesh.normals[p + 1];
            vertices[i].nz = mesh.normals[p + 2];
        }
        vertices[i].u = (t + 1 < mesh.uvs.size()) ? mesh.uvs[t + 0] : 0.0f;
        vertices[i].v = (t + 1 < mesh.uvs.size()) ? mesh.uvs[t + 1] : 0.0f;
        vertices[i].material_id = default_material_id;
    }
    return vertices;
}

CombinedScene SceneManager::UploadCombined(
    VulkanEngine::Runtime::VulkanBootstrap& bootstrap,
    const std::vector<LoadedMeshData>& meshes,
    const std::vector<uint16_t>& material_ids_for_meshes,
    uint16_t default_material_id) {
    CombinedScene scene;
    auto& backend = bootstrap.GetBackend();

    // Convert all meshes to vertices and combine
    std::vector<VulkanEngine::StandardMeshPipeline::Vertex> all_vertices;
    std::vector<uint32_t> all_indices;
    uint32_t vertex_offset = 0;
    uint32_t index_offset = 0;

    for (size_t i = 0; i < meshes.size(); ++i) {
        const uint16_t mat_id = (i < material_ids_for_meshes.size()) ? material_ids_for_meshes[i] : default_material_id;
        auto verts = ConvertToVertices(meshes[i], mat_id);

        MeshInfo info{};
        info.name = "mesh_" + std::to_string(i);
        info.vertex_offset = vertex_offset;
        info.vertex_count = static_cast<uint32_t>(verts.size());
        info.index_offset = index_offset;
        info.index_count = static_cast<uint32_t>(meshes[i].indices.size());

        scene.meshes.push_back(info);

        // Copy vertices
        all_vertices.insert(all_vertices.end(), verts.begin(), verts.end());

        // Copy indices with vertex offset adjustment
        for (const uint32_t idx : meshes[i].indices) {
            all_indices.push_back(idx + vertex_offset);
        }

        vertex_offset += static_cast<uint32_t>(verts.size());
        index_offset += static_cast<uint32_t>(meshes[i].indices.size());
    }

    if (all_vertices.empty()) return scene;

    // Upload combined vertex buffer
    scene.vertex_buffer = VulkanEngine::GpuResources::GpuBuffer::Create(
        backend,
        all_vertices.size() * sizeof(VulkanEngine::StandardMeshPipeline::Vertex),
        vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
        all_vertices.data());

    // Upload combined index buffer
    scene.index_buffer = VulkanEngine::GpuResources::GpuBuffer::Create(
        backend,
        all_indices.size() * sizeof(uint32_t),
        vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
        all_indices.data());

    LOGIFACE_LOG(info, "UploadCombined: " + std::to_string(scene.meshes.size()) + " meshes, " +
                 std::to_string(all_vertices.size()) + " vertices, " +
                 std::to_string(all_indices.size()) + " indices");

    return scene;
}

}
