module;

#include <memory>
#include <future>
#include <array>
#include <vector>
#include <cstdint>
#include <cstddef>
#include <bit>
#include <cstring>
#include <stdexcept>
#include <string>
#include <FileLoader/FileLoader.hpp>
#include <FileLoader/Types.hpp>

export module VulkanEngine.FileLoaders.Mesh.BinMeshAssembler;

import VulkanEngine.Mesh.MeshTypes;

// helpers (non-exported)
namespace {
    template<typename T>
    T ReadValue(const FileLoader::ByteBuffer& buffer, size_t& off, const std::string& field_name)
    {
        if (off + sizeof(T) > buffer.size()) {
            throw std::out_of_range("BinMeshAssembler: buffer too small to read " + field_name +
                                    " (offset: " + std::to_string(off) +
                                    ", expected: " + std::to_string(sizeof(T)) +
                                    ", total size: " + std::to_string(buffer.size()) + ")");
        }
        using Arr = std::array<std::byte, sizeof(T)>;
        Arr a;
        std::memcpy(a.data(), buffer.data() + off, sizeof(T));
        off += sizeof(T);
        return std::bit_cast<T>(a);
    }

    inline float ReadFloat(const FileLoader::ByteBuffer& buffer, size_t& off, const std::string& name) { return ReadValue<float>(buffer, off, name); }
    inline uint32_t ReadU32(const FileLoader::ByteBuffer& buffer, size_t& off, const std::string& name) { return ReadValue<uint32_t>(buffer, off, name); }
    inline uint16_t ReadU16(const FileLoader::ByteBuffer& buffer, size_t& off, const std::string& name) { return ReadValue<uint16_t>(buffer, off, name); }
    inline uint8_t ReadU8(const FileLoader::ByteBuffer& buffer, size_t& off, const std::string& name) { return ReadValue<uint8_t>(buffer, off, name); }

    void ReadVector3(const FileLoader::ByteBuffer& buffer, size_t& off, VulkanEngine::Vector3& v, const std::string& prefix) {
        v.x = ReadFloat(buffer, off, prefix + ".x");
        v.y = ReadFloat(buffer, off, prefix + ".y");
        v.z = ReadFloat(buffer, off, prefix + ".z");
    }

    void ReadVector2(const FileLoader::ByteBuffer& buffer, size_t& off, VulkanEngine::Vector2& v, const std::string& prefix) {
        v.u = ReadFloat(buffer, off, prefix + ".u");
        v.v = ReadFloat(buffer, off, prefix + ".v");
    }

    void ReadSubMesh(const FileLoader::ByteBuffer& buffer, size_t& off, VulkanEngine::SubMesh& sm, uint32_t index) {
        const std::string p = "subMesh[" + std::to_string(index) + "]";
        sm.index_start = ReadU32(buffer, off, p + ".index_start");
        sm.index_count = ReadU32(buffer, off, p + ".index_count");
        sm.material_index = ReadU16(buffer, off, p + ".material_index");
    }

    void ReadBoneWeight(const FileLoader::ByteBuffer& buffer, size_t& off, VulkanEngine::BoneWeight& bw, uint32_t index) {
        const std::string p = "boneWeight[" + std::to_string(index) + "]";
        for (size_t i = 0; i < 4; ++i) bw.bone_indices[i] = ReadU16(buffer, off, p + ".bone_indices[" + std::to_string(i) + "]");
        for (size_t i = 0; i < 4; ++i) bw.weights[i] = ReadU8(buffer, off, p + ".weights[" + std::to_string(i) + "]");
    }
}

export namespace App::FileLoaders::Mesh {

class BinMeshAssembler : public FileLoader::IAssembler<VulkanEngine::Mesh, FileLoader::AssemblyMode::FullBuffer>
{
public:
    std::future<std::shared_ptr<VulkanEngine::Mesh>> AssembleFromFullBuffer(std::shared_ptr<FileLoader::ByteBuffer> buffer) override {
        auto prom = std::make_shared<std::promise<std::shared_ptr<VulkanEngine::Mesh>>>();
        try {
            size_t off = 0;

            // 1. Verify Magic Number (3 bytes: 0x1B, 0xEF, 0xF8)
            auto m0 = ReadU8(*buffer, off, "magic[0]");
            auto m1 = ReadU8(*buffer, off, "magic[1]");
            auto m2 = ReadU8(*buffer, off, "magic[2]");
            if (m0 != 0x1B || m1 != 0xEF || m2 != 0xF8) {
                throw std::runtime_error("BinMeshAssembler: Invalid magic number");
            }

            // 2. Skip Materials using the new size header
            const uint16_t materialCount = ReadU16(*buffer, off, "materialCount");
            for (uint16_t i = 0; i < materialCount; ++i) {
                const uint16_t materialSize = ReadU16(*buffer, off, "material[" + std::to_string(i) + "].size");
                if (off + materialSize > buffer->size()) {
                    throw std::out_of_range("BinMeshAssembler: buffer too small to skip material[" + std::to_string(i) + "]");
                }
                off += materialSize;
            }

            // 3. Read Mesh Count
            const uint16_t meshCount = ReadU16(*buffer, off, "meshCount");
            if (meshCount == 0) {
                throw std::runtime_error("BinMeshAssembler: No meshes found in file");
            }

            // 4. Parse first mesh
            auto mesh = std::make_shared<VulkanEngine::Mesh>();

            const uint32_t vertexCount = ReadU32(*buffer, off, "vertexCount");

            mesh->vertices.resize(vertexCount);
            for (uint32_t i = 0; i < vertexCount; ++i) ReadVector3(*buffer, off, mesh->vertices[i], "vertex[" + std::to_string(i) + "]");

            mesh->normals.resize(vertexCount);
            for (uint32_t i = 0; i < vertexCount; ++i) ReadVector3(*buffer, off, mesh->normals[i], "normal[" + std::to_string(i) + "]");

            mesh->uvs.resize(vertexCount);
            for (uint32_t i = 0; i < vertexCount; ++i) ReadVector2(*buffer, off, mesh->uvs[i], "uv[" + std::to_string(i) + "]");

            const uint32_t indexCount = ReadU32(*buffer, off, "indexCount");
            mesh->indices.resize(indexCount);
            for (uint32_t i = 0; i < indexCount; ++i) mesh->indices[i] = ReadU32(*buffer, off, "index[" + std::to_string(i) + "]");

            const uint32_t subMeshCount = ReadU32(*buffer, off, "subMeshCount");
            mesh->subMeshes.resize(subMeshCount);
            for (uint32_t i = 0; i < subMeshCount; ++i) ReadSubMesh(*buffer, off, mesh->subMeshes[i], i);

            prom->set_value(mesh);
        } catch (const std::exception& e) {
            prom->set_exception(std::current_exception());
        }
        return prom->get_future();
    }
};

class BinSkinnedMeshAssembler : public FileLoader::IAssembler<VulkanEngine::SkinnedMesh, FileLoader::AssemblyMode::FullBuffer>
{
public:
    std::future<std::shared_ptr<VulkanEngine::SkinnedMesh>> AssembleFromFullBuffer(std::shared_ptr<FileLoader::ByteBuffer> buffer) override {
        auto prom = std::make_shared<std::promise<std::shared_ptr<VulkanEngine::SkinnedMesh>>>();
        try {
            size_t off = 0;

            // Skip Magic (3 bytes)
            off += 3;

            const uint16_t materialCount = ReadU16(*buffer, off, "materialCount");
            for (uint16_t i = 0; i < materialCount; ++i) {
                const uint16_t materialSize = ReadU16(*buffer, off, "material[" + std::to_string(i) + "].size");
                off += materialSize;
            }


            auto mesh = std::make_shared<VulkanEngine::SkinnedMesh>();

            const uint32_t vertexCount = ReadU32(*buffer, off, "vertexCount");

            mesh->vertices.resize(vertexCount);
            for (uint32_t i = 0; i < vertexCount; ++i) ReadVector3(*buffer, off, mesh->vertices[i], "vertex[" + std::to_string(i) + "]");

            mesh->normals.resize(vertexCount);
            for (uint32_t i = 0; i < vertexCount; ++i) ReadVector3(*buffer, off, mesh->normals[i], "normal[" + std::to_string(i) + "]");

            mesh->uvs.resize(vertexCount);
            for (uint32_t i = 0; i < vertexCount; ++i) ReadVector2(*buffer, off, mesh->uvs[i], "uv[" + std::to_string(i) + "]");

            const uint32_t indexCount = ReadU32(*buffer, off, "indexCount");
            mesh->indices.resize(indexCount);
            for (uint32_t i = 0; i < indexCount; ++i) mesh->indices[i] = ReadU32(*buffer, off, "index[" + std::to_string(i) + "]");

            const uint32_t subMeshCount = ReadU32(*buffer, off, "subMeshCount");
            mesh->subMeshes.resize(subMeshCount);
            for (uint32_t i = 0; i < subMeshCount; ++i) ReadSubMesh(*buffer, off, mesh->subMeshes[i], i);

            const uint32_t boneWeightCount = ReadU32(*buffer, off, "boneWeightCount");
            mesh->bone_weights.resize(boneWeightCount);
            for (uint32_t i = 0; i < boneWeightCount; ++i) ReadBoneWeight(*buffer, off, mesh->bone_weights[i], i);

            prom->set_value(mesh);
        } catch (const std::exception& e) {
            prom->set_exception(std::current_exception());
        }
        return prom->get_future();
    }
};

} // namespace App::FileLoaders::Mesh
