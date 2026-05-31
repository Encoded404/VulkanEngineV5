// Per-submesh transform constants written by the expand pass
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

struct VSOutput {
    [[vk::location(0)]] float3 outN : TEXCOORD0;
    [[vk::location(1)]] float2 outT : TEXCOORD1;
    [[vk::location(2)]] uint outM : TEXCOORD2;
    float4 position : SV_Position;
};

[shader("vertex")]
VSOutput main(uint vertexId : SV_VertexID) {
    // vertexId indexes into indirectBuffer which maps to (vertexBuffer, submesh) pairs
    VSOutput result;
    IndirEntry entry = indirectBuffer[vertexId];
    uint bufferIndex = entry.vid >> 24;
    uint vertexIndex = entry.vid & 0xFFFFFFu;
    uint submeshId = entry.sid;
    uint block = submeshId / 256u;
    uint elemIndex = submeshId % 256u;
    Vertex vert = vertexBuffers[bufferIndex][vertexIndex];
    float3 p = float3(vert.px, vert.py, vert.pz);
    float3 n = float3(vert.nx, vert.ny, vert.nz);
    float2 t = float2(vert.u, vert.v);

    VertEntry info = submeshInfo[NonUniformResourceIndex(block)][elemIndex];
    result.outM = info.texSlot;
    result.outN = n;
    result.outT = t;
    result.position = mul(float4(p, 1.0), info.MVP);
    return result;
}
