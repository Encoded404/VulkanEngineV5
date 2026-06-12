module;

export module VulkanEngine.Mesh.MeshTypes;

import std;

export import VulkanEngine.MaterialManager.MaterialId;
export import VulkanEngine.TechniqueManager.TechniqueId;
export import VulkanEngine.BindlessManager.TextureSlot;

export namespace VulkanEngine
{
    using MaterialManager::MaterialId;

    class MeshVertexVec3
    {
    public:
        float x, y, z; //NOLINT(misc-non-private-member-variables-in-classes)
    };

    class MeshVertexVec2
    {
    public:
        float u, v; //NOLINT(misc-non-private-member-variables-in-classes)
    };

    struct BoundingSphere
    {
        MeshVertexVec3 center{};
        float radius{0.0f};
    };

    struct BoundingOBB
    {
        MeshVertexVec3 center{};
        float pad0{0.0f};
        MeshVertexVec3 axis_u{};
        float half_extent_u{0.0f};
        MeshVertexVec3 axis_v{};
        float half_extent_v{0.0f};
        MeshVertexVec3 axis_w{};
        float half_extent_w{0.0f};
    };

    struct SubMesh
    {
        std::uint32_t index_start{0}; //NOLINT(misc-non-private-member-variables-in-classes)
        std::uint32_t index_count{0}; //NOLINT(misc-non-private-member-variables-in-classes)
        MaterialId material_id{0}; //NOLINT(misc-non-private-member-variables-in-classes)
        BoundingSphere sphere{};
        BoundingOBB obb{};
    };

    struct BoneWeight
    {
        std::array<std::uint16_t, 4> bone_indices{}; //NOLINT(misc-non-private-member-variables-in-classes)
        std::array<std::uint8_t, 4> weights{}; //NOLINT(misc-non-private-member-variables-in-classes)
    };

    class Mesh
    {
    public:
        std::vector<MeshVertexVec3> vertices; //NOLINT(misc-non-private-member-variables-in-classes)
        std::vector<MeshVertexVec3> normals; //NOLINT(misc-non-private-member-variables-in-classes)
        std::vector<MeshVertexVec2> uvs; //NOLINT(misc-non-private-member-variables-in-classes)
        std::vector<std::uint32_t> indices; //NOLINT(misc-non-private-member-variables-in-classes)
        std::vector<SubMesh> subMeshes; //NOLINT(misc-non-private-member-variables-in-classes)
    };

    struct SkinnedMesh : public Mesh
    {
        std::vector<BoneWeight> bone_weights; //NOLINT(misc-non-private-member-variables-in-classes)
    };
} // namespace VulkanEngine
