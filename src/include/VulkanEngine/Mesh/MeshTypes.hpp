#ifndef VULKAN_ENGINE_MESH_TYPES_HPP
#define VULKAN_ENGINE_MESH_TYPES_HPP

#include <FileLoader/FileLoader.hpp>
#include <array>
#include <vector>
#include <cstdint>
#include <cstddef>
#include <algorithm>
#include <bit>
#include <stdexcept>

// small helpers for reading typed values from a ByteBuffer while advancing an offset
namespace {
    template<typename T>
    T readValue(const FileLoader::ByteBuffer& buffer, size_t& off)
    {
        using Arr = std::array<std::byte, sizeof(T)>;
        if (off + sizeof(T) > buffer.size()) throw std::out_of_range("readValue: buffer too small");
        Arr a;
        std::copy_n(buffer.begin() + off, sizeof(T), a.begin());
        off += sizeof(T);
        return std::bit_cast<T>(a);
    }

    inline float readFloat(const FileLoader::ByteBuffer& buffer, size_t& off) { return readValue<float>(buffer, off); }
    inline uint32_t readU32(const FileLoader::ByteBuffer& buffer, size_t& off) { return readValue<uint32_t>(buffer, off); }
    inline uint16_t readU16(const FileLoader::ByteBuffer& buffer, size_t& off) { return readValue<uint16_t>(buffer, off); }
    inline uint8_t readU8(const FileLoader::ByteBuffer& buffer, size_t& off) { return readValue<uint8_t>(buffer, off); }
}

namespace VulkanEngine
{
    class Vector3
    {
    public:
        float x, y, z;
        void FromBuffer(const FileLoader::ByteBuffer& buffer, size_t offset)
        {
            size_t off = offset;
            x = readFloat(buffer, off);
            y = readFloat(buffer, off);
            z = readFloat(buffer, off);
        }
    };

    class Vector2
    {
    public:
        float u, v;
        void FromBuffer(const FileLoader::ByteBuffer& buffer, size_t offset)
        {
            size_t off = offset;
            u = readFloat(buffer, off);
            v = readFloat(buffer, off);
        }
    };

    struct SubMesh
    {
        uint32_t index_start{0};
        uint32_t index_count{0};
        uint16_t material_index{0};

        void FromBuffer(const FileLoader::ByteBuffer& buffer, size_t offset)
        {
            size_t off = offset;
            index_start = readU32(buffer, off);
            index_count = readU32(buffer, off);
            material_index = readU16(buffer, off);
        }
    };

    struct boneWeight
    {
        std::array<uint16_t, 4> bone_indices{};
        std::array<uint8_t, 4> weights{};

        void FromBuffer(const FileLoader::ByteBuffer& buffer, size_t offset)
        {
            size_t off = offset;
            for (size_t i = 0; i < 4; ++i)
            {
                bone_indices[i] = readU16(buffer, off);
                weights[i] = readU8(buffer, off);
            }
        }
    };

    class Mesh
    {
    public:
        std::vector<Vector3> vertices;
        std::vector<Vector3> normals;
        std::vector<Vector2> uvs;
        std::vector<uint32_t> indices;
        std::vector<SubMesh> subMeshes;

        void FromBuffer(const FileLoader::ByteBuffer& buffer, size_t offset)
        {
            size_t off = offset;

            // vertex count
            uint32_t vertexCount = readU32(buffer, off);
            vertices.resize(vertexCount);
            for (uint32_t i = 0; i < vertexCount; ++i)
            {
                // read 3 floats
                vertices[i].FromBuffer(buffer, off);
                // Vector3::FromBuffer advances its own local offset; our local 'off' has been advanced by helpers
            }

            // normals
            normals.resize(vertexCount);
            for (uint32_t i = 0; i < vertexCount; ++i)
            {
                normals[i].FromBuffer(buffer, off);
            }

            // uvs
            uvs.resize(vertexCount);
            for (uint32_t i = 0; i < vertexCount; ++i)
            {
                uvs[i].FromBuffer(buffer, off);
            }

            // index count
            uint32_t indexCount = readU32(buffer, off);
            indices.resize(indexCount);
            for (uint32_t i = 0; i < indexCount; ++i)
            {
                indices[i] = readU32(buffer, off);
            }

            // submesh count
            uint32_t subMeshCount = readU32(buffer, off);
            subMeshes.resize(subMeshCount);
            for (uint32_t i = 0; i < subMeshCount; ++i)
            {
                subMeshes[i].FromBuffer(buffer, off);
                // helpers advanced 'off' already inside SubMesh::FromBuffer
            }
        }
    };

    struct skinnedMesh : public Mesh
    {
        std::vector<boneWeight> bone_weights;

        void FromBuffer(const FileLoader::ByteBuffer& buffer, size_t offset)
        {
            // first parse base mesh
            Mesh::FromBuffer(buffer, offset);
            // compute offset where bone weights begin: re-parse the structure using helpers
            size_t off = offset;

            uint32_t vertexCount = readU32(buffer, off);
            off += static_cast<size_t>(vertexCount) * (sizeof(float) * 3); // positions
            off += static_cast<size_t>(vertexCount) * (sizeof(float) * 3); // normals
            off += static_cast<size_t>(vertexCount) * (sizeof(float) * 2); // uvs

            uint32_t indexCount = readU32(buffer, off);
            off += static_cast<size_t>(indexCount) * sizeof(uint32_t);

            uint32_t subMeshCount = readU32(buffer, off);
            off += static_cast<size_t>(subMeshCount) * (4 + 4 + 2);

            uint32_t boneWeightCount = readU32(buffer, off);
            bone_weights.resize(boneWeightCount);
            for (uint32_t i = 0; i < boneWeightCount; ++i)
            {
                bone_weights[i].FromBuffer(buffer, off);
            }
        }
    };
} // namespace VulkanEngine

#endif // VULKAN_ENGINE_MESH_TYPES_HPP