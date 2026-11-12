// GPU probe for the PackTBN mirror (engine/core/shaders/normal_pack.slang).
// Packs a grid of (normal, tangent, handedness) triples so the device-free test
// can compare the words bit-for-bit with the CPU encoder.
//
// The GPU test compiler has no include search path, so the engine header is
// included by relative path; Slang resolves includes against the including
// file.
#include "../../../engine/core/shaders/normal_pack.slang"

struct PackInput {
    float4 normal;   // xyz = normal
    float4 tangent;  // xyz = tangent, w = handedness (+1 / -1)
};

[[vk::binding(0, 0)]]
StructuredBuffer<PackInput> inputs;

[[vk::binding(1, 0)]]
RWStructuredBuffer<uint> outputs;

struct PackPC {
    uint elementCount;
};

[[vk::push_constant]]
ConstantBuffer<PackPC> packPC;

[numthreads(64, 1, 1)]
[shader("compute")]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint i = dispatchThreadId.x;
    if (i >= packPC.elementCount) return;
    PackInput v = inputs[i];
    outputs[i] = PackTBN(v.normal.xyz, v.tangent.xyz, v.tangent.w);
}
