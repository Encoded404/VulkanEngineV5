#include "scene_entries.slang"

[[vk::binding(0, 1)]]
StructuredBuffer<VertexEntry, CDataLayout> submeshVertexEntries[];

[[vk::binding(0, 2)]]
StructuredBuffer<Vertex, CDataLayout> vertexBuffers[];

[[vk::binding(0, 3)]]
StructuredBuffer<VertexIndirectionEntry, CDataLayout> vertexIndirection;

#include "normal_encoding.slang"

struct VSOutput {
    [[vk::location(0)]] float3 outN : TEXCOORD0;
    [[vk::location(1)]] float2 outT : TEXCOORD1;
    [[vk::location(2)]] nointerpolation uint outM : TEXCOORD2;
    [[vk::location(3)]] float3 outW : TEXCOORD3;    // world-space position
    [[vk::location(4)]] nointerpolation uint outOrm : TEXCOORD4;    // packed unorm8 ORM factors
    [[vk::location(5)]] float3 outTangent : TEXCOORD5;   // world-space tangent
    [[vk::location(6)]] float outHandedness : TEXCOORD6; // bitangent handedness, +1/-1
    float4 position : SV_Position;
};

[shader("vertex")]
VSOutput main(uint vertexId : SV_VertexID) {
    // Hardware indexed draw: SV_VertexID is the value from draw_indices, which
    // is an absolute slot into vertex_indirection (vertexOffset is always 0).
    VSOutput result;
    VertexIndirectionEntry entry = vertexIndirection[vertexId];
    uint vertexBufferSlot = entry.vertexIndex >> 24;
    uint vertexIndex = entry.vertexIndex & 0xFFFFFFu;
    uint submeshIndex = entry.submeshIndex;
    uint block = submeshIndex / 256u;
    uint elementIndex = submeshIndex % 256u;
    Vertex vert = vertexBuffers[NonUniformResourceIndex(vertexBufferSlot)][vertexIndex];
    float3 p = float3(vert.px, vert.py, vert.pz);
    float2 t = float2(vert.u, vert.v);

    // Unpack the tangent frame in model space, then transform both vectors
    // with the per-instance normal matrix (inverse-transpose). Renormalize
    // after each decode/transform step.
    float3 n_ms;
    float4 t_ms = UnpackTBN(vert.packedTBN, n_ms);

    VertexEntry info = submeshVertexEntries[NonUniformResourceIndex(block)][elementIndex];
    result.outM = info.materialId;
    result.outOrm = info.ormPacked;
    result.outN = normalize(mul(n_ms, info.normalMatrix));
    float3 tangent_ws = normalize(mul(t_ms.xyz, info.normalMatrix));
    result.outTangent = tangent_ws;
    result.outHandedness = t_ms.w;
    result.outT = t;
    result.outW = mul(float4(p, 1.0), info.modelMatrix).xyz;
    result.position = mul(float4(p, 1.0), info.mvp);
    return result;
}


