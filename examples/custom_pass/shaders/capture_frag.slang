[[vk::binding(0, 5)]]
Sampler2D backbuffer;

struct PSInput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

// Copies the backbuffer into the transient scene-colour image. This reads the
// acquired swapchain image in the fragment stage, which is what the acquire
// semaphore wait must cover.
[shader("fragment")]
float4 main(PSInput input) : SV_TARGET {
    return backbuffer.Sample(input.uv);
}
