// Per-submesh transform constants (same layout as main vertex shader)
struct VertEntry {
    float4x4 MVP;
    float maxScale;
    uint texSlot;
};

[[vk::binding(0, 1)]]
StructuredBuffer<VertEntry> submeshInfo[];

struct Vertex {
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
};

[[vk::binding(0, 2)]]
StructuredBuffer<Vertex> vertexBuffers[];

struct IndirEntry {
    uint vid;
    uint sid;
};

[[vk::binding(0, 3)]]
StructuredBuffer<IndirEntry> indirectBuffer;

[shader("vertex")]
float4 main(uint vertexId : SV_VertexID) : SV_Position {
    IndirEntry entry = indirectBuffer[vertexId];
    uint bufferIndex = entry.vid >> 24;
    uint vertexIndex = entry.vid & 0xFFFFFFu;
    uint submeshId = entry.sid;
    uint block = submeshId / 256u;
    uint elemIndex = submeshId % 256u;
    Vertex vert = vertexBuffers[bufferIndex][vertexIndex];
    float3 p = float3(vert.px, vert.py, vert.pz);
    return mul(float4(p, 1.0), submeshInfo[NonUniformResourceIndex(block)][elemIndex].MVP);
}
