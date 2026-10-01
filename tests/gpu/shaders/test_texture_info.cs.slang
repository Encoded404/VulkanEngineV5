// Reads the bindless GpuTextureInfo buffer (set 0, binding 1) and copies one
// slot's metadata to an output buffer (set 1, binding 0). The struct is
// byte-identical to the C++ VulkanEngine::Textures::GpuTextureInfo mirror.

struct GpuTextureInfo {
    uint width;
    uint height;
    uint mip_levels;
    uint array_layers;
    uint format;
    uint flags;
};

[[vk::binding(1, 0)]]
StructuredBuffer<GpuTextureInfo, CDataLayout> textureInfo;

[[vk::binding(0, 1)]]
RWStructuredBuffer<uint> outInfo;

[shader("compute")]
[numthreads(1, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    const GpuTextureInfo info = textureInfo[tid.x];
    const uint base = tid.x * 6u;
    outInfo[base + 0u] = info.width;
    outInfo[base + 1u] = info.height;
    outInfo[base + 2u] = info.mip_levels;
    outInfo[base + 3u] = info.array_layers;
    outInfo[base + 4u] = info.format;
    outInfo[base + 5u] = info.flags;
}
