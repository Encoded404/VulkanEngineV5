[[vk::binding(0, 0)]]
Texture2D<float4> allTextures[];
[[vk::binding(0, 0)]]
SamplerState defaultSampler;

struct PSInput {
    [[vk::location(0)]] float3 normal : TEXCOORD0;
    [[vk::location(1)]] float2 texCoord : TEXCOORD1;
    [[vk::location(2)]] uint materialId : TEXCOORD2;
};

[shader("fragment")]
float4 main(PSInput input) : SV_TARGET {
    float4 texColor = allTextures[NonUniformResourceIndex(input.materialId)].Sample(defaultSampler, input.texCoord);
    if (texColor.a < 0.1) {
        discard;
    }
    return texColor;
}
