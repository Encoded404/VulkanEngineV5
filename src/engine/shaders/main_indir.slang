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

struct VSOutput {
    [[vk::location(0)]] float3 outN : TEXCOORD0;
    [[vk::location(1)]] float2 outT : TEXCOORD1;
    [[vk::location(2)]] uint outM : TEXCOORD2;
    float4 position : SV_Position;
};

[shader("vertex")]
VSOutput main(uint vertexId : SV_VertexID) {
    VSOutput result;
    IndirEntry e = ib[vertexId];
    uint bufIdx = e.vid >> 24;
    uint vertIdx = e.vid & 0xFFFFFFu;
    uint submeshId = e.sid;
    uint block = submeshId / 256u;
    uint elemIdx = submeshId % 256u;
    Vertex v = vb[bufIdx][vertIdx];
    float3 p = float3(v.px, v.py, v.pz);
    float3 n = float3(v.nx, v.ny, v.nz);
    float2 t = float2(v.u, v.v);

    VertEntry s = sv[NonUniformResourceIndex(block)][elemIdx];
    result.outM = s.textureSlot;
    result.outN = n;
    result.outT = t;
    result.position = mul(s.MVP, float4(p, 1.0));
    return result;
}
