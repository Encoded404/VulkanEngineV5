#include "scene_entries.slang"

[[vk::binding(0, 1)]]
StructuredBuffer<VertexEntry, CDataLayout> submeshVertexEntries[];

[[vk::binding(0, 2)]]
StructuredBuffer<Vertex, CDataLayout> vertexBuffers[];

[[vk::binding(0, 3)]]
StructuredBuffer<VertexIndirectionEntry, CDataLayout> vertexIndirection;

[shader("vertex")]
float4 main(uint vertexId : SV_VertexID) : SV_Position {
    VertexIndirectionEntry entry = vertexIndirection[vertexId];
    uint vertexBufferSlot = entry.vertexIndex >> 24;
    uint vertexIndex = entry.vertexIndex & 0xFFFFFFu;
    uint submeshIndex = entry.submeshIndex;
    uint block = submeshIndex / 256u;
    uint elementIndex = submeshIndex % 256u;
    Vertex vert = vertexBuffers[NonUniformResourceIndex(vertexBufferSlot)][vertexIndex];
    float3 p = float3(vert.px, vert.py, vert.pz);
    return mul(float4(p, 1.0), submeshVertexEntries[NonUniformResourceIndex(block)][elementIndex].mvp);
}

