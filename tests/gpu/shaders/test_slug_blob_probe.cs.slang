// Test-only readback for the hb-gpu blob storage buffer.
//
// The blob is a sequence of 8-byte units, each unit four signed 16-bit values.
// This probe declares the buffer with 8-byte elements exactly the way the Slug
// fragment shader's adapted `hb_gpu_fetch` does -- `StructuredBuffer<int2>`, NOT
// the 16-byte `StructuredBuffer<int4>` the upstream HLSL header comment
// suggests -- and writes both the raw words and the sign-extended int4 back, so
// the test can compare each against the blob's real bytes. A 16-byte stride
// would make element i+1 read the wrong unit and fail the raw comparison.

[[vk::binding(0, 0)]]
StructuredBuffer<int2, CDataLayout> hb_gpu_atlas;

[[vk::binding(1, 0)]]
RWStructuredBuffer<int> probeOut;

struct SlugProbeParams {
    uint base_element;   // first blob unit to copy, in the buffer's own units
    uint element_count;  // how many units the blob occupies
    uint probe_element;  // unit within the blob to sign-extend
    uint pad0;
};

[[vk::push_constant]]
SlugProbeParams params;

// 32-bit-only sign extension of the four 16-bit halves of one 8-byte unit. No
// int16_t appears, so the shader does not need 16-bit storage or arithmetic
// support; the shifts are the whole adaptation.
int4 hb_gpu_fetch(int offset) {
    const int2 raw = hb_gpu_atlas[offset];
    return int4((raw.x << 16) >> 16, raw.x >> 16,
                (raw.y << 16) >> 16, raw.y >> 16);
}

[shader("compute")]
[numthreads(1, 1, 1)]
void main() {
    for (uint element = 0u; element < params.element_count; ++element) {
        const int2 raw = hb_gpu_atlas[params.base_element + element];
        probeOut[2u * element] = raw.x;
        probeOut[2u * element + 1u] = raw.y;
    }
    const int4 extended =
        hb_gpu_fetch(int(params.base_element + params.probe_element));
    const uint base = 2u * params.element_count;
    probeOut[base + 0u] = extended.x;
    probeOut[base + 1u] = extended.y;
    probeOut[base + 2u] = extended.z;
    probeOut[base + 3u] = extended.w;
}
