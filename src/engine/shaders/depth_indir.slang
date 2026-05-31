struct VertEntry {
    float4x4 MVP;
    float maxScale;
    uint textureSlot;
};

[[vk::binding(0, 1)]]
StructuredBuffer<VertEntry> sv[];

struct Vertex {
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
};

[[vk::binding(0, 2)]]
StructuredBuffer<Vertex> vb[];

struct IndirEntry {
    uint vid;
    uint sid;
};

[[vk::binding(0, 3)]]
StructuredBuffer<IndirEntry> ib;

[shader("vertex")]
float4 main(uint vertexId : SV_VertexID) : SV_Position {
    IndirEntry e = ib[vertexId];
    uint bufIdx = e.vid >> 24;
    uint vertIdx = e.vid & 0xFFFFFFu;
    uint submeshId = e.sid;
    uint block = submeshId / 256u;
    uint elemIdx = submeshId % 256u;
    Vertex v = vb[bufIdx][vertIdx];
    float3 p = float3(v.px, v.py, v.pz);
    return mul(float4(p, 1.0), sv[NonUniformResourceIndex(block)][elemIdx].MVP);
}