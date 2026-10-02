#include "scene_entries.slang"
#include "technique_packing.slang"   // kCompactionMode (constant_id 0): draw-mode selector

[[vk::binding(0, 1)]]
StructuredBuffer<VertexEntry, CDataLayout> submeshVertexEntries[];

[[vk::binding(0, 2)]]
StructuredBuffer<Vertex, CDataLayout> vertexBuffers[];

// Out-of-line UV set 1 (engine set 2, binding 1). Block-addressed like the
// vertex table; a sentinel uvSlot in the entry means the set is absent and the
// shader emits UV0 for it.
[[vk::binding(1, 2)]]
StructuredBuffer<float2, CDataLayout> uvBuffers[];

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

// Variant output with the out-of-line UV1 interpolant. Only this entry point's
// pipeline declares it, so the UV0-only path pays nothing.
struct VSOutputUV1 {
    [[vk::location(0)]] float3 outN : TEXCOORD0;
    [[vk::location(1)]] float2 outT : TEXCOORD1;
    [[vk::location(2)]] nointerpolation uint outM : TEXCOORD2;
    [[vk::location(3)]] float3 outW : TEXCOORD3;
    [[vk::location(4)]] nointerpolation uint outOrm : TEXCOORD4;
    [[vk::location(5)]] float3 outTangent : TEXCOORD5;
    [[vk::location(6)]] float outHandedness : TEXCOORD6;
    [[vk::location(7)]] float2 outUV1 : TEXCOORD7;
    float4 position : SV_Position;
};

// Shared body: decodes the vertex (both draw modes), transforms the tangent
// frame, and returns the world-space interpolants. `absVertex` is the absolute
// vertex index the UV re-base uses.
struct VertexDecode {
    float4x4 mvp;
    float3 position_ms;
    float3 position_ws;
    float3 normal_ws;
    float3 tangent_ws;
    float  handedness;
    float2 uv0;
    float2 uv1;
    uint   material_id;
    uint   orm_packed;
};

VertexDecode DecodeVertex(uint vertexId, uint submeshIndex, bool use_uv1) {
    VertexDecode d;
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

    float3 n_ms;
    float4 t_ms = UnpackTBN(vert.packedTBN, n_ms);

    VertexEntry info = submeshVertexEntries[NonUniformResourceIndex(block)][elementIndex];
    d.mvp = info.mvp;
    d.material_id = info.materialId;
    d.orm_packed = info.ormPacked;
    d.normal_ws = normalize(mul(n_ms, info.normalMatrix));
    d.tangent_ws = normalize(mul(t_ms.xyz, info.normalMatrix));
    d.handedness = t_ms.w;
    d.uv0 = float2(vert.u, vert.v);
    d.uv1 = d.uv0;
    // mvp already contains the model transform, so the clip position must be fed
    // the model-space position; position_ws is only for world-space interpolants.
    d.position_ms = p;
    d.position_ws = mul(float4(p, 1.0), info.modelMatrix).xyz;
    if (use_uv1 && info.uvSlot != kNoUvSlot) {
        // uvBias re-bases the absolute vertex index into the UV region's own
        // element space. vertexIndex is already baseVertex + rawIndex in both
        // modes, so one formula serves CID and MID.
        d.uv1 = uvBuffers[NonUniformResourceIndex(info.uvSlot)][info.uvBias + (int)vertexIndex];
    }
    return d;
}

[shader("vertex")]
VSOutput main(uint vertexId : SV_VertexID,
              uint submeshIndex : SV_StartInstanceLocation) {
    VSOutput result;
    VertexDecode d = DecodeVertex(vertexId, submeshIndex, false);
    result.outM = d.material_id;
    result.outOrm = d.orm_packed;
    result.outN = d.normal_ws;
    result.outTangent = d.tangent_ws;
    result.outHandedness = d.handedness;
    result.outT = d.uv0;
    result.outW = d.position_ws;
    result.position = mul(float4(d.position_ms, 1.0), d.mvp);
    return result;
}

// UV1 variant: identical to main, plus the out-of-line UV1 interpolant. The
// fragment shader's matching entry point samples the texture through it.
[shader("vertex")]
VSOutputUV1 main_uv1(uint vertexId : SV_VertexID,
                     uint submeshIndex : SV_StartInstanceLocation) {
    VSOutputUV1 result;
    VertexDecode d = DecodeVertex(vertexId, submeshIndex, true);
    result.outM = d.material_id;
    result.outOrm = d.orm_packed;
    result.outN = d.normal_ws;
    result.outTangent = d.tangent_ws;
    result.outHandedness = d.handedness;
    result.outT = d.uv0;
    result.outUV1 = d.uv1;
    result.outW = d.position_ws;
    result.position = mul(float4(d.position_ms, 1.0), d.mvp);
    return result;
}


