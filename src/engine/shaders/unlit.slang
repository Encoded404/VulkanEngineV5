// Unlit texture shader.
//
// Samples the per-material albedo texture from the engine bindless array
// (set 0, binding 0) and outputs it directly — no lighting, no scene header.
// Useful for screen-space quads, UI panels, webcam/decals and debug views.
// The pipeline layout intentionally contains the engine sets (0-4) and the
// technique's PerMaterial set (5); this shader simply uses a subset of them.

[[vk::binding(0, 0)]]
Texture2D<float4> allTextures[];
[[vk::binding(0, 0)]]
SamplerState defaultSampler;

// Per-material data (set 5, binding 0) — must match
// TechniqueManager::DefaultMeshPerMaterialData. C layout: 28 bytes.
struct MaterialData {
    uint albedo_texture;
    uint normal_texture;
    uint orm_texture;
    float roughness_factor;
    float metallic_factor;
    float ao_factor;
};

[[vk::binding(0, 5)]]
StructuredBuffer<MaterialData, CDataLayout> materialBuffer[];

struct PSInput {
    [[vk::location(0)]] float3 normal : TEXCOORD0;
    [[vk::location(1)]] float2 texCoord : TEXCOORD1;
    [[vk::location(2)]] uint materialId : TEXCOORD2;
    [[vk::location(3)]] float3 worldPos : TEXCOORD3;
};

[shader("fragment")]
float4 main(PSInput input) : SV_TARGET {
    uint block = input.materialId / 256u;
    uint elem  = input.materialId % 256u;
    MaterialData mat = materialBuffer[NonUniformResourceIndex(block)][elem];

    float4 albedo = allTextures[NonUniformResourceIndex(mat.albedo_texture)]
        .Sample(defaultSampler, input.texCoord);
    //albedo.x = 1.0;
    //albedo.z = 1.0;
    //albedo = float4(1.0, 0.0, 0.5, 1.0);
    return albedo;
}
