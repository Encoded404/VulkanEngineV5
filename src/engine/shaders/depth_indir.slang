// Per-submesh transform constants (same layout as main vertex shader).
// C layout: byte-identical to the C++ mirror in MeshGatherSystem.cpp (176 B).
struct VertEntry {
    float4x4 MVP;
    float maxScale;
    uint materialId;
    uint ormPacked;         // packed by expand pass, unused by depth pass
    float4x4 modelMatrix;   // unused by depth pass, must match main_indir size
    float3x3 normalMatrix;  // unused by depth pass, must match main_indir size
};

[[vk::binding(0, 1)]]
StructuredBuffer<VertEntry, CDataLayout> submeshInfo[];

struct Vertex {
    float px, py, pz;
    uint packedTBN;         // unused by depth pass (position-only fetch), layout must match main_indir
    float u, v;
};

[[vk::binding(0, 2)]]
StructuredBuffer<Vertex, CDataLayout> vertexBuffers[];

struct IndirEntry {
    uint vid;
    uint sid;
};

[[vk::binding(0, 3)]]
StructuredBuffer<IndirEntry, CDataLayout> indirectBuffer;

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

