[[vk::binding(0, 5)]]
Sampler2D sceneColor;

struct PSInput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

// Copies the engine's linear scene target into the example's transient image.
// The final display encode happens later, in the engine's output pass.
[shader("fragment")]
float4 main(PSInput input) : SV_TARGET {
    return sceneColor.Sample(input.uv);
}
