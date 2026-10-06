// Test-only readback for an RGBA8 glyph-atlas page (the MSDF page).
//
// The page image is created by GpuTexture::CreateStream, which is
// eTransferDst | eSampled and deliberately NOT eTransferSrc, so a test cannot
// copyImageToBuffer it. It samples the page exactly the way the world-text
// shader will -- through the bindless array, at texel centres so the linear
// sampler returns the stored texel -- and writes all four channels out so the
// test can compare them against the generated field.

[[vk::binding(0, 0)]]
Sampler2D<float4> allTextures[];

// Readback destination: one uint4 per texel, each channel scaled to 0..255.
[[vk::binding(0, 1)]]
RWStructuredBuffer<uint4> outTexels;

struct ReadParams {
    uint slot;
    uint width;
    uint height;
};

[[vk::push_constant]]
ReadParams params;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= params.width || id.y >= params.height) {
        return;
    }
    const float2 uv = (float2(id.xy) + 0.5) / float2(params.width, params.height);
    const float4 sampled = allTextures[NonUniformResourceIndex(params.slot)].Sample(uv);
    outTexels[id.y * params.width + id.x] =
        uint4(round(saturate(sampled) * 255.0));
}
