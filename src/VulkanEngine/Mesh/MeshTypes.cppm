module;

#include <array>
#include <vector>
#include <cstdint>

export module VulkanEngine.Mesh.MeshTypes;

export namespace VulkanEngine
{
    class Vector3
    {
    public:
        float x, y, z; //NOLINT(misc-non-private-member-variables-in-classes)
    };

    class Vector2
    {
    public:
        float u, v; //NOLINT(misc-non-private-member-variables-in-classes)
    };

    struct SubMesh
    {
        uint32_t index_start{0}; //NOLINT(misc-non-private-member-variables-in-classes)
        uint32_t index_count{0}; //NOLINT(misc-non-private-member-variables-in-classes)
        uint16_t material_index{0}; //NOLINT(misc-non-private-member-variables-in-classes)
    };

    struct BoneWeight
    {
        std::array<uint16_t, 4> bone_indices{}; //NOLINT(misc-non-private-member-variables-in-classes)
        std::array<uint8_t, 4> weights{}; //NOLINT(misc-non-private-member-variables-in-classes)
    };

    class Mesh
    {
    public:
        std::vector<Vector3> vertices; //NOLINT(misc-non-private-member-variables-in-classes)
        std::vector<Vector3> normals; //NOLINT(misc-non-private-member-variables-in-classes)
        std::vector<Vector2> uvs; //NOLINT(misc-non-private-member-variables-in-classes)
        std::vector<uint32_t> indices; //NOLINT(misc-non-private-member-variables-in-classes)
        std::vector<SubMesh> subMeshes; //NOLINT(misc-non-private-member-variables-in-classes)
    };

    struct SkinnedMesh : public Mesh
    {
        std::vector<BoneWeight> bone_weights; //NOLINT(misc-non-private-member-variables-in-classes)
    };
} // namespace VulkanEngine
