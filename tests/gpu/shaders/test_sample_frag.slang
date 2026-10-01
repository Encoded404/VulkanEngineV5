// Test-only fragment shader: returns the raw sampled RGBA of bindless slot 0.
//
// The compressed-format tests compare this output for a BCn upload against the
// same fixture uploaded as RGBA8, so every channel (including alpha) is visible
// and no lighting/ORM factor masks the result. The uploaded bytes and the
// sampler are the production GpuTexture path; only the trivial read is local to
// the test.

[[vk::binding(0, 0)]]
Sampler2D<float4> allTextures[];

struct PSInput {
    [[vk::location(1)]] float2 texCoord : TEXCOORD1;
};

[shader("fragment")]
float4 main(PSInput input) : SV_TARGET {
    return allTextures[NonUniformResourceIndex(0u)].Sample(input.texCoord);
}
