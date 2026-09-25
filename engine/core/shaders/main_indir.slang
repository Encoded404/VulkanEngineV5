#include "scene_entries.slang"
#include "technique_packing.slang"   // kCompactionMode (constant_id 0): draw-mode selector

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
VSOutput main(uint vertexId : SV_VertexID,
              uint submeshIndex : SV_StartInstanceLocation) {
    // CID: draw_indices holds absolute slots into vertexIndirection
    // (vertexOffset is always 0) and the submesh id comes from the entry.
    // MID: draw_indices holds absolute vertex indices, so SV_VertexID is the
    // vertex index directly, and firstInstance (the raw instance builtin, NOT
    // the zero-based SV_InstanceID) is the submesh id.
    VSOutput result;
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


