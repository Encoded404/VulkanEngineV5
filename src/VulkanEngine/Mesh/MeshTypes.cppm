module;

#include <array>
#include <vector>
#include <cstdint>
#include <cstddef>
#include <algorithm>
#include <bit>
#include <stdexcept>
#include <FileLoader/Types.hpp>

export module VulkanEngine.Mesh.MeshTypes;

// helpers (non-exported)
namespace {
    template<typename T>
    T ReadValue(const FileLoader::ByteBuffer& buffer, size_t& off)
    {
        using Arr = std::array<std::byte, sizeof(T)>;
        if (off + sizeof(T) > buffer.size()) throw std::out_of_range("readValue: buffer too small");
        Arr a;
        std::copy_n(buffer.begin() + off, sizeof(T), a.begin());
        off += sizeof(T);
        return std::bit_cast<T>(a);
    }

    inline float ReadFloat(const FileLoader::ByteBuffer& buffer, size_t& off) { return ReadValue<float>(buffer, off); }
    inline uint32_t ReadU32(const FileLoader::ByteBuffer& buffer, size_t& off) { return ReadValue<uint32_t>(buffer, off); }
    inline uint16_t ReadU16(const FileLoader::ByteBuffer& buffer, size_t& off) { return ReadValue<uint16_t>(buffer, off); }
    inline uint8_t ReadU8(const FileLoader::ByteBuffer& buffer, size_t& off) { return ReadValue<uint8_t>(buffer, off); }
}

export namespace VulkanEngine
{
    class Vector3
    {
    public:
        float x, y, z; //NOLINT(misc-non-private-member-variables-in-classes)
        void FromBuffer(const FileLoader::ByteBuffer& buffer, size_t offset)
        {
            size_t off = offset;
            x = ReadFloat(buffer, off);
            y = ReadFloat(buffer, off);
            z = ReadFloat(buffer, off);
        }
    };

    class Vector2
    {
    public:
        float u, v; //NOLINT(misc-non-private-member-variables-in-classes)
        void FromBuffer(const FileLoader::ByteBuffer& buffer, size_t offset)
        {
            size_t off = offset;
            u = ReadFloat(buffer, off);
            v = ReadFloat(buffer, off);
        }
    };

    struct SubMesh
    {
        uint32_t index_start{0}; //NOLINT(misc-non-private-member-variables-in-classes)
        uint32_t index_count{0}; //NOLINT(misc-non-private-member-variables-in-classes)
        uint16_t material_index{0}; //NOLINT(misc-non-private-member-variables-in-classes)

        void FromBuffer(const FileLoader::ByteBuffer& buffer, size_t offset)
        {
            size_t off = offset;
            index_start = ReadU32(buffer, off);
            index_count = ReadU32(buffer, off);
            material_index = ReadU16(buffer, off);
        }
    };

    struct BoneWeight
    {
        std::array<uint16_t, 4> bone_indices{}; //NOLINT(misc-non-private-member-variables-in-classes)
        std::array<uint8_t, 4> weights{}; //NOLINT(misc-non-private-member-variables-in-classes)

        void FromBuffer(const FileLoader::ByteBuffer& buffer, size_t offset)
        {
            size_t off = offset;
            for (size_t i = 0; i < 4; ++i)
            {
                bone_indices[i] = ReadU16(buffer, off);
                weights[i] = ReadU8(buffer, off);
            }
        }
    };

    class Mesh
    {
    public:
        std::vector<Vector3> vertices; //NOLINT(misc-non-private-member-variables-in-classes)
        std::vector<Vector3> normals; //NOLINT(misc-non-private-member-variables-in-classes)
        std::vector<Vector2> uvs; //NOLINT(misc-non-private-member-variables-in-classes)
        std::vector<uint32_t> indices; //NOLINT(misc-non-private-member-variables-in-classes)
        std::vector<SubMesh> subMeshes; //NOLINT(misc-non-private-member-variables-in-classes)

        void FromBuffer(const FileLoader::ByteBuffer& buffer, size_t offset)
        {
            size_t off = offset;

            const uint32_t vertexCount = ReadU32(buffer, off);
            vertices.resize(vertexCount);
            for (uint32_t i = 0; i < vertexCount; ++i)
            {
                vertices[i].FromBuffer(buffer, off);
            }

            normals.resize(vertexCount);
            for (uint32_t i = 0; i < vertexCount; ++i)
            {
                normals[i].FromBuffer(buffer, off);
            }

            uvs.resize(vertexCount);
            for (uint32_t i = 0; i < vertexCount; ++i)
            {
                uvs[i].FromBuffer(buffer, off);
            }

            const uint32_t indexCount = ReadU32(buffer, off);
            indices.resize(indexCount);
            for (uint32_t i = 0; i < indexCount; ++i)
            {
                indices[i] = ReadU32(buffer, off);
            }

            const uint32_t subMeshCount = ReadU32(buffer, off);
            subMeshes.resize(subMeshCount);
            for (uint32_t i = 0; i < subMeshCount; ++i)
            {
                subMeshes[i].FromBuffer(buffer, off);
            }
        }
    };

    struct SkinnedMesh : public Mesh
    {
        std::vector<BoneWeight> bone_weights; //NOLINT(misc-non-private-member-variables-in-classes)

        void FromBuffer(const FileLoader::ByteBuffer& buffer, size_t offset)
        {
            Mesh::FromBuffer(buffer, offset);
            size_t off = offset;

            const uint32_t vertexCount = ReadU32(buffer, off);
            off += static_cast<size_t>(vertexCount) * (sizeof(float) * 3);
            off += static_cast<size_t>(vertexCount) * (sizeof(float) * 3);
            off += static_cast<size_t>(vertexCount) * (sizeof(float) * 2);

            const uint32_t indexCount = ReadU32(buffer, off);
            off += static_cast<size_t>(indexCount) * sizeof(uint32_t);

            const uint32_t subMeshCount = ReadU32(buffer, off);
            off += static_cast<size_t>(subMeshCount) * (4 + 4 + 2);

            const uint32_t boneWeightCount = ReadU32(buffer, off);
            bone_weights.resize(boneWeightCount);
            for (uint32_t i = 0; i < boneWeightCount; ++i)
            {
                bone_weights[i].FromBuffer(buffer, off);
            }
        }
    };
} // namespace VulkanEngine
