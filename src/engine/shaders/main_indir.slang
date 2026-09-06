// Per-submesh transform constants written by the expand pass.
// C layout: byte-identical to the C++ mirror in MeshGatherSystem.cpp (176 B);
// float3x3 = 3 tightly packed 12-byte rows, matrices not 16B-aligned.
struct VertEntry {
    float4x4 MVP;
    float maxScale;
    uint materialId;
    uint ormPacked;         // unorm8 ORM factors packed by the CPU gather pass
    float4x4 modelMatrix;   // world matrix for per-vertex world-space position
    float3x3 normalMatrix;  // inverse-transpose of the 3x3 of modelMatrix;
                            // computed analytically in the expand pass
};

[[vk::binding(0, 1)]]
StructuredBuffer<VertEntry, CDataLayout> submeshInfo[];

struct Vertex {
    float px, py, pz;
    uint packedTBN;         // packed tangent frame, see normal_encoding.slang
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

#include "normal_encoding.slang"

struct VSOutput {
    [[vk::location(0)]] float3 outN : TEXCOORD0;
    [[vk::location(1)]] float2 outT : TEXCOORD1;
    [[vk::location(2)]] uint outM : TEXCOORD2;
    [[vk::location(3)]] float3 outW : TEXCOORD3;    // world-space position
    [[vk::location(4)]] uint outOrm : TEXCOORD4;    // packed unorm8 ORM factors
    [[vk::location(5)]] float3 outTangent : TEXCOORD5;   // world-space tangent
    [[vk::location(6)]] float outHandedness : TEXCOORD6; // bitangent handedness, +1/-1
    float4 position : SV_Position;
};

[shader("vertex")]
VSOutput main(uint vertexId : SV_VertexID, uint baseVertex : SV_StartVertexLocation) {
    // vertexId indexes into indirectBuffer which maps to (vertexBuffer, submesh) pairs
    VSOutput result;
    IndirEntry entry = indirectBuffer[vertexId + baseVertex];
    uint bufferIndex = entry.vid >> 24;
    uint vertexIndex = entry.vid & 0xFFFFFFu;
    uint submeshId = entry.sid;
    uint block = submeshId / 256u;
    uint elemIndex = submeshId % 256u;
    Vertex vert = vertexBuffers[bufferIndex][vertexIndex];
    float3 p = float3(vert.px, vert.py, vert.pz);
    float2 t = float2(vert.u, vert.v);

    // Unpack the tangent frame in model space, then transform both vectors
    // with the per-instance normal matrix (inverse-transpose). Renormalize
    // after each decode/transform step.
    float3 n_ms;
    float4 t_ms = UnpackTBN(vert.packedTBN, n_ms);

    VertEntry info = submeshInfo[NonUniformResourceIndex(block)][elemIndex];
    result.outM = info.materialId;
    result.outOrm = info.ormPacked;
    result.outN = normalize(mul(n_ms, info.normalMatrix));
    float3 tangent_ws = normalize(mul(t_ms.xyz, info.normalMatrix));
    result.outTangent = tangent_ws;
    result.outHandedness = t_ms.w;
    result.outT = t;
    result.outW = mul(float4(p, 1.0), info.modelMatrix).xyz;
    result.position = mul(float4(p, 1.0), info.MVP);
    return result;
}


