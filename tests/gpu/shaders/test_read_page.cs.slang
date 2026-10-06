// Test-only readback for the glyph-atlas GPU page.
//
// The page image is created by GpuTexture::CreateStream, which is
// eTransferDst | eSampled and deliberately NOT eTransferSrc, so a test cannot
// copyImageToBuffer it. It samples the page exactly the way a text shader will
// -- through the bindless array, at texel centres so the linear sampler returns
// the stored texel -- and writes the R channel (the A8 coverage) out as bytes so
// the test can compare it against RasterGlyph::coverage.

[[vk::binding(0, 0)]]
Sampler2D<float4> allTextures[];

// Readback destination: one uint per texel, R channel scaled to 0..255.
[[vk::binding(0, 1)]]
RWStructuredBuffer<uint> outCoverage;

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
    outCoverage[id.y * params.width + id.x] = uint(round(saturate(sampled.r) * 255.0));
}
