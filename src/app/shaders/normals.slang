struct PSInput {
    [[vk::location(0)]] float3 normal : TEXCOORD0;
};

[shader("fragment")]
float4 main(PSInput input) : SV_TARGET {
    return float4(normalize(input.normal) * 0.5 + 0.5, 1.0);
}
