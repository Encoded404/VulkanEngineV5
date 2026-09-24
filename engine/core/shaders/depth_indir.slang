#include "scene_entries.slang"
#include "technique_packing.slang"   // kCompactionMode (constant_id 0): draw-mode selector

[[vk::binding(0, 1)]]
StructuredBuffer<VertexEntry, CDataLayout> submeshVertexEntries[];

[[vk::binding(0, 2)]]
StructuredBuffer<Vertex, CDataLayout> vertexBuffers[];

[[vk::binding(0, 3)]]
StructuredBuffer<VertexIndirectionEntry, CDataLayout> vertexIndirection;

[shader("vertex")]
float4 main(uint vertexId : SV_VertexID,
            uint submeshIndex : SV_StartInstanceLocation) : SV_Position {
    // See main_indir.slang: monolithic reads the indirection entry, MID takes
    // the vertex index from SV_VertexID and the submesh id from the raw
    // instance builtin (never the zero-based SV_InstanceID).
    uint vertexIndex;
    uint vertexBufferSlot;
    uint block;
    uint elementIndex;
    if (kCompactionMode == 0) {
        VertexIndirectionEntry entry = vertexIndirection[vertexId];
        vertexBufferSlot = entry.vertexIndex >> 24;
        vertexIndex = entry.vertexIndex & 0xFFFFFFu;
        submeshIndex = entry.submeshIndex;
        block = submeshIndex / 256u;
        elementIndex = submeshIndex % 256u;
    } else {
        vertexIndex = vertexId;
        block = submeshIndex / 256u;
        elementIndex = submeshIndex % 256u;
        vertexBufferSlot =
            submeshVertexEntries[NonUniformResourceIndex(block)][elementIndex].vertexBufferSlot;
    }
    Vertex vert = vertexBuffers[NonUniformResourceIndex(vertexBufferSlot)][vertexIndex];
    float3 p = float3(vert.px, vert.py, vert.pz);
    return mul(float4(p, 1.0), submeshVertexEntries[NonUniformResourceIndex(block)][elementIndex].mvp);
}

