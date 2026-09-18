[[vk::binding(0, 5)]]
Sampler2D sceneColor;

// Exposure written by the compute pass earlier in the frame.
[[vk::binding(1, 5)]]
StructuredBuffer<float> exposure;

struct PSInput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("fragment")]
float4 main(PSInput input) : SV_TARGET {
    float3 color = max(sceneColor.Sample(input.uv).rgb, 0.0);
    const float e = exposure[0];
    color = 1.0 - exp(-color * e);
    return float4(pow(saturate(color), 1.0 / 2.2), 1.0);
}
