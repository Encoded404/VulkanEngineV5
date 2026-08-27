module;

export module VulkanEngine.GpuResources.MeshData;

import std;

export import VulkanEngine.StandardMeshPipeline;
export import VulkanEngine.Mesh.MeshTypes;

export namespace VulkanEngine::GpuResources {

struct MeshData {
    std::vector<StandardMeshPipeline::Vertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<SubMesh> sub_meshes;
};

// Ensures every submesh carries a valid bounding volume (sphere + OBB) in
// mesh-local space. Submeshes with an already-valid sphere (radius > 0) are
// left untouched; submeshes with degenerate bounds are recomputed from the
// vertex/index data. If the mesh has geometry but no submeshes, one spanning
// all indices is created first. Idempotent and cheap to call at registration
// time, so CPU-generated meshes (e.g. fallback quads) that bypass the file
// loader still reach the GPU with correct culling bounds.
void EnsureSubmeshBounds(MeshData& data) {
    if (data.vertices.empty() || data.indices.empty()) return;

    if (data.sub_meshes.empty()) {
        SubMesh sm{};
        sm.index_start = 0;
        sm.index_count = static_cast<std::uint32_t>(data.indices.size());
        data.sub_meshes.push_back(sm);
    }

    std::vector<MeshVertexVec3> positions;
    positions.reserve(data.vertices.size());
    for (const auto& v : data.vertices) {
        positions.push_back(MeshVertexVec3{v.px, v.py, v.pz});
    }

    for (auto& sm : data.sub_meshes) {
        if (sm.sphere.radius > 0.0f) continue;
        ComputeSubmeshBounds(sm, positions, data.indices);
    }
}

class IMeshSource {
public:
    virtual ~IMeshSource() = default;
    virtual MeshData Load() = 0;
};

} // namespace VulkanEngine::GpuResources
