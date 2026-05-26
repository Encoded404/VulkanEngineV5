module;

#include <memory>
#include <future>
#include <vector>
#include <string>
#include <stdexcept>
#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <FileLoader/FileLoader.hpp>
#include <FileLoader/Types.hpp>
#define TINYOBJLOADER_IMPLEMENTATION
#include <tiny_obj_loader.h>

export module VulkanEngine.FileLoaders.Mesh.ObjMeshAssembler;

import VulkanEngine.Mesh.MeshTypes;
import VulkanEngine.FileLoaders.Mesh.MeshLoaderBase;

export namespace VulkanEngine::FileLoaders::Mesh {

class ObjMeshAssembler : public FileLoader::IAssembler<VulkanEngine::Mesh, FileLoader::AssemblyMode::FullBuffer>
{
public:
    std::future<std::shared_ptr<VulkanEngine::Mesh>> AssembleFromFullBuffer(std::shared_ptr<FileLoader::ByteBuffer> buffer) override {
        auto prom = std::make_shared<std::promise<std::shared_ptr<VulkanEngine::Mesh>>>();
        try {
            auto mesh = std::make_shared<VulkanEngine::Mesh>();

            const std::string obj_str(reinterpret_cast<const char*>(buffer->data()), buffer->size());

            tinyobj::ObjReader reader;
            tinyobj::ObjReaderConfig config;
            config.triangulate = true;
            if (!reader.ParseFromString(obj_str, "", config)) {
                throw std::runtime_error("ObjMeshAssembler: " + reader.Error());
            }

            const auto& attrib = reader.GetAttrib();
            const auto& shapes = reader.GetShapes();

            struct ObjVert {
                // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
                int vi, ni, ti;
                // NOLINTEND(misc-non-private-member-variables-in-classes)
                bool operator==(const ObjVert& o) const {
                    return vi == o.vi && ni == o.ni && ti == o.ti;
                }
            };
            struct ObjVertHash {
                size_t operator()(const ObjVert& v) const {
                    return static_cast<size_t>(v.vi) ^ (static_cast<size_t>(v.ni) << 10) ^ (static_cast<size_t>(v.ti) << 20);
                }
            };

            uint32_t base_index = 0;
            for (const auto& shape : shapes) {
                std::unordered_map<ObjVert, uint32_t, ObjVertHash> vert_map;
                size_t index_offset = 0;

                for (size_t f = 0; f < shape.mesh.num_face_vertices.size(); ++f) {
                    const uint32_t fv = shape.mesh.num_face_vertices[f];
                    for (uint32_t v = 0; v < fv; ++v) {
                        const auto& idx = shape.mesh.indices[index_offset + v];
                        const ObjVert key{idx.vertex_index, idx.normal_index, idx.texcoord_index};
                        const auto it = vert_map.find(key);
                        if (it == vert_map.end()) {
                            const uint32_t new_idx = static_cast<uint32_t>(mesh->vertices.size());
                            vert_map[key] = new_idx;

                            const float px = idx.vertex_index >= 0 ? attrib.vertices[3 * static_cast<size_t>(idx.vertex_index) + 0] : 0.0f;
                            const float py = idx.vertex_index >= 0 ? attrib.vertices[3 * static_cast<size_t>(idx.vertex_index) + 1] : 0.0f;
                            const float pz = idx.vertex_index >= 0 ? attrib.vertices[3 * static_cast<size_t>(idx.vertex_index) + 2] : 0.0f;
                            mesh->vertices.push_back({px, py, pz});

                            const float nx = idx.normal_index >= 0 ? attrib.normals[3 * static_cast<size_t>(idx.normal_index) + 0] : 0.0f;
                            const float ny = idx.normal_index >= 0 ? attrib.normals[3 * static_cast<size_t>(idx.normal_index) + 1] : 0.0f;
                            const float nz = idx.normal_index >= 0 ? attrib.normals[3 * static_cast<size_t>(idx.normal_index) + 2] : 0.0f;
                            mesh->normals.push_back({nx, ny, nz});

                            const float tu = idx.texcoord_index >= 0 ? attrib.texcoords[2 * static_cast<size_t>(idx.texcoord_index) + 0] : 0.0f;
                            const float tv = idx.texcoord_index >= 0 ? attrib.texcoords[2 * static_cast<size_t>(idx.texcoord_index) + 1] : 0.0f;
                            mesh->uvs.push_back({tu, tv});
                        }
                        mesh->indices.push_back(vert_map[key]);
                    }
                    index_offset += static_cast<size_t>(fv);
                }

                const uint32_t submesh_count = static_cast<uint32_t>(mesh->indices.size()) - base_index;
                if (submesh_count > 0) {
                    mesh->subMeshes.push_back(VulkanEngine::SubMesh{
                        .index_start = base_index,
                        .index_count = submesh_count,
                    });
                }
                base_index = static_cast<uint32_t>(mesh->indices.size());
            }

            if (mesh->vertices.empty()) {
                throw std::runtime_error("ObjMeshAssembler: No vertices loaded from OBJ file");
            }

            prom->set_value(mesh);
        } catch (...) {
            prom->set_exception(std::current_exception());
        }
        return prom->get_future();
    }
};

class ObjMeshLoader : public IMeshLoader {
public:
    ObjMeshLoader() = default;

protected:
    std::shared_ptr<VulkanEngine::Mesh> DoLoad(const std::filesystem::path& path) override {
        auto buf = ReadEntireFile(path);
        auto buf_ptr = std::make_shared<FileLoader::ByteBuffer>(buf.begin(), buf.end());
        ObjMeshAssembler assembler;
        return assembler.AssembleFromFullBuffer(std::move(buf_ptr)).get();
    }

private:
    static std::vector<std::byte> ReadEntireFile(const std::filesystem::path& file_path) {
        std::ifstream file(file_path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            throw std::runtime_error("ObjMeshLoader: Failed to open file: " + file_path.string());
        }
        const auto size = static_cast<size_t>(file.tellg());
        if (size == 0) {
            throw std::runtime_error("ObjMeshLoader: File is empty: " + file_path.string());
        }
        std::vector<std::byte> buf(size);
        file.seekg(0, std::ios::beg);
        if (!file.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(size))) {
            throw std::runtime_error("ObjMeshLoader: Failed to read file: " + file_path.string());
        }
        return buf;
    }
};

} // namespace VulkanEngine::FileLoaders::Mesh
