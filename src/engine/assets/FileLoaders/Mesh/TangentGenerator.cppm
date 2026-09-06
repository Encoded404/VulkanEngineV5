module;

#include <mikktspace.h>

export module VulkanEngine.Mesh.TangentGenerator;

import std;

import VulkanEngine.Mesh.MeshTypes;

export namespace VulkanEngine::TangentGen {

// Generates per-vertex MikkTSpace tangents for one submesh of an indexed mesh.
// The mesh must already be deduplicated (shared vertices share index entries)
// so tangents accumulate across triangle fans correctly.
//
// Results are written to out_tangents / out_handedness, sized index_count
// (unindexed per MikkTSpace's output contract) and are then scattered back
// onto the referenced vertices by this function. If a vertex is referenced by
// faces with conflicting tangent spaces (true UV seams), the last face wins;
// MikkTSpace's internal welding already averages within smooth groups, so
// conflicts here are rare and confined to hard UV borders.
//
// Returns false if MikkTSpace rejected the geometry (e.g. no valid UVs).
[[nodiscard]] inline bool GenerateSubmeshTangents(
    const std::vector<MeshVertexVec3>& positions,
    const std::vector<MeshVertexVec3>& normals,
    const std::vector<MeshVertexVec2>& uvs,
    const std::vector<std::uint32_t>& indices,
    std::uint32_t index_start,
    std::uint32_t index_count,
    std::vector<MeshVertexVec3>& out_tangents,     // per-vertex, must be sized positions.size()
    std::vector<float>& out_handedness)            // per-vertex, +1.0f / -1.0f
{
    if (positions.empty() || uvs.empty() || index_count < 3) return false;

    // Scratch buffers for the unindexed MikkTSpace output.
    std::vector<MeshVertexVec3> face_tangents(index_count);
    std::vector<float> face_signs(index_count);

    struct Context {
        const std::vector<MeshVertexVec3>* positions;
        const std::vector<MeshVertexVec3>* normals;
        const std::vector<MeshVertexVec2>* uvs;
        const std::uint32_t* indices;
        std::uint32_t index_start;
        std::uint32_t index_count;
        MeshVertexVec3* face_tangents;
        float* face_signs;
    } ctx{
        .positions = &positions,
        .normals = &normals,
        .uvs = &uvs,
        .indices = indices.data(),
        .index_start = index_start,
        .index_count = index_count,
        .face_tangents = face_tangents.data(),
        .face_signs = face_signs.data(),
    };

    SMikkTSpaceInterface iface{};
    iface.m_getNumFaces = [](const SMikkTSpaceContext* pContext) -> int {
        auto* c = static_cast<Context*>(pContext->m_pUserData);
        return static_cast<int>(c->index_count / 3);
    };
    iface.m_getNumVerticesOfFace = [](const SMikkTSpaceContext*, const int) -> int {
        return 3; // assemblers triangulate on load
    };
    iface.m_getPosition = [](const SMikkTSpaceContext* pContext, float fvPosOut[], int iFace, int iVert) {
        auto* c = static_cast<Context*>(pContext->m_pUserData);
        const MeshVertexVec3& p = (*c->positions)[c->indices[c->index_start + iFace * 3 + iVert]];
        fvPosOut[0] = p.x; fvPosOut[1] = p.y; fvPosOut[2] = p.z;
    };
    iface.m_getNormal = [](const SMikkTSpaceContext* pContext, float fvNormOut[], int iFace, int iVert) {
        auto* c = static_cast<Context*>(pContext->m_pUserData);
        const MeshVertexVec3& n = (*c->normals)[c->indices[c->index_start + iFace * 3 + iVert]];
        fvNormOut[0] = n.x; fvNormOut[1] = n.y; fvNormOut[2] = n.z;
    };
    iface.m_getTexCoord = [](const SMikkTSpaceContext* pContext, float fvTexcOut[], int iFace, int iVert) {
        auto* c = static_cast<Context*>(pContext->m_pUserData);
        const MeshVertexVec2& t = (*c->uvs)[c->indices[c->index_start + iFace * 3 + iVert]];
        fvTexcOut[0] = t.u; fvTexcOut[1] = t.v;
    };
    iface.m_setTSpaceBasic = [](const SMikkTSpaceContext* pContext, const float fvTangent[], const float fSign, int iFace, int iVert) {
        auto* c = static_cast<Context*>(pContext->m_pUserData);
        const std::uint32_t out = iFace * 3 + iVert;
        c->face_tangents[out] = {fvTangent[0], fvTangent[1], fvTangent[2]};
        c->face_signs[out] = fSign;
    };

    SMikkTSpaceContext mikk_ctx{};
    mikk_ctx.m_pInterface = &iface;
    mikk_ctx.m_pUserData = &ctx;

    if (!genTangSpaceDefault(&mikk_ctx)) return false;

    // Scatter unindexed results onto vertices. Faces that share a vertex
    // within a smooth group produce identical tangents (MikkTSpace welding);
    // hard UV seams intentionally keep divergent values per face-corner, but
    // our vertices are deduplicated across the whole submesh, so a true seam
    // requires the loader to have split vertices at the UV border — which
    // tinyobj dedup (pos+normal+uv keying) already guarantees.
    for (std::uint32_t i = 0; i < index_count; ++i) {
        const std::uint32_t v = indices[index_start + i];
        out_tangents[v] = face_tangents[i];
        out_handedness[v] = face_signs[i];
    }
    return true;
}

} // namespace VulkanEngine::TangentGen
