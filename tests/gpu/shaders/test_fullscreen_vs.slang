// Test-only vertex shader for the offscreen texture-render harness.
//
// It emits a full-screen triangle and the exact VS->FS interface the engine's
// standard_mesh fragment shader declares (locations 0..6), so the REAL shipped
// fragment shader can be exercised without the whole scene/collect pipeline.
// A plane facing +Z with a fixed tangent frame is enough for both the texture
// sampling tests (which ignore the frame) and the normal-mapping tests (which
// perturb N from a sampled normal map).

struct VSOutput {
    [[vk::location(0)]] float3 outN : TEXCOORD0;
    [[vk::location(1)]] float2 outT : TEXCOORD1;
    [[vk::location(2)]] nointerpolation uint outM : TEXCOORD2;
    [[vk::location(3)]] float3 outW : TEXCOORD3;
    [[vk::location(4)]] nointerpolation uint outOrm : TEXCOORD4;
    [[vk::location(5)]] float3 outTangent : TEXCOORD5;
    [[vk::location(6)]] float outHandedness : TEXCOORD6;
    float4 position : SV_Position;
};

[shader("vertex")]
VSOutput main(uint vertexId : SV_VertexID) {
    // Classic full-screen triangle: (-1,-1), (3,-1), (-1,3).
    float x = (vertexId == 1) ? 3.0 : -1.0;
    float y = (vertexId == 2) ? 3.0 : -1.0;

    VSOutput o;
    o.position = float4(x, y, 0.0, 1.0);
    o.outN = float3(0.0, 0.0, 1.0);
    o.outTangent = float3(1.0, 0.0, 0.0);
    o.outHandedness = 1.0;
    // UV spans 0..1 across the visible [-1,1] region.
    o.outT = float2(x, y) * 0.5 + 0.5;
    o.outW = float3(x, y, 0.0);
    o.outM = 0u;
    // ao=1, roughness=1, metallic=0 (unorm8 packing, matching MeshGatherSystem).
    o.outOrm = 0xFFu | (0xFFu << 8) | (0u << 16);
    return o;
}
