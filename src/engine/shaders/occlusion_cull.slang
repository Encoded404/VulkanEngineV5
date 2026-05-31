// Per-submesh transform and material info (written by expand pass)
struct VertEntry {
    float4x4 MVP;
    float maxScale;
    uint texSlot;
};

[[vk::binding(0, 0)]]
StructuredBuffer<VertEntry> submeshVertexInfo[];

struct CullEntry {
    uint indirOffset;
    uint indexCount;
    uint techniqueId;
    uint pad;
};

[[vk::binding(1, 0)]]
RWStructuredBuffer<CullEntry> cullEntries[];

[[vk::binding(2, 0)]]
StructuredBuffer<float4> boundingSpheres[];

[[vk::binding(3, 0)]]
Texture2D<float> hizTex;
[[vk::binding(3, 0)]]
SamplerState hizSampler;

// Oriented bounding box: center + 3 axis-aligned half-extent vectors (xyz = axis, w = half-length)
struct OBBGPU {
    float4 center_pad;
    float4 u_half;
    float4 v_half;
    float4 w_half;
};

[[vk::binding(4, 0)]]
StructuredBuffer<OBBGPU> obbBuf[];

struct OccPC {
    uint elementCount;
    uint refineLevel;
    uint hizWidth;
    uint hizHeight;
};

[[vk::push_constant]]
ConstantBuffer<OccPC> pc;

[numthreads(64, 1, 1)]
[shader("compute")]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint id = dispatchThreadId.x;
    if (id >= pc.elementCount) return;

    uint block = id / 256u;
    uint elemIndex = id % 256u;

    uint currentIndexCount = cullEntries[NonUniformResourceIndex(block)][elemIndex].indexCount;
    if (currentIndexCount == 0) return;

    float4x4 mvp = submeshVertexInfo[NonUniformResourceIndex(block)][elemIndex].MVP;
    float maxScaleVal = submeshVertexInfo[NonUniformResourceIndex(block)][elemIndex].maxScale;

    // bounding sphere test: cheap early-out before OBB test
    float4 sphere = boundingSpheres[NonUniformResourceIndex(block)][elemIndex];
    float3 sphereCenter = sphere.xyz;
    float localRadius = sphere.w;

    float4 clipPos = mul(float4(sphereCenter, 1.0), mvp);
    if (clipPos.w <= 0.0) {
        cullEntries[NonUniformResourceIndex(block)][elemIndex].indexCount = 0;
        return;
    }

    float worldRadius = localRadius * maxScaleVal;

    float frontW = max(clipPos.w - worldRadius, 0.001);
    float nearZ = (clipPos.z - worldRadius) / frontW;

    float screenRadius = abs(worldRadius * mvp[1][1] / clipPos.w);
    int mip = int(floor(log2(max(screenRadius, 1.0))));
    mip = min(mip, int(floor(log2(max(float(pc.hizWidth), float(pc.hizHeight))))));

    float3 ndc = clipPos.xyz / clipPos.w;
    float2 screenPos = ndc.xy * 0.5 + 0.5;
    screenPos.y = 1.0 - screenPos.y;

    float hizDepthVal = hizTex.SampleLevel(hizSampler, screenPos, float(mip)).r;
    if (nearZ > hizDepthVal + 0.001) {
        cullEntries[NonUniformResourceIndex(block)][elemIndex].indexCount = 0;
        return;
    }

    OBBGPU obb = obbBuf[NonUniformResourceIndex(block)][elemIndex];

    int cornerCount = 0;
    float nearZ_OBB = 3.402823e+38;
    float2 minXY = float2(3.402823e+38, 3.402823e+38);
    float2 maxXY = float2(-3.402823e+38, -3.402823e+38);

    float3 center  = obb.center_pad.xyz;
    float3 axisU   = obb.u_half.xyz;
    float halfU    = obb.u_half.w;
    float3 axisV   = obb.v_half.xyz;
    float halfV    = obb.v_half.w;
    float3 axisW   = obb.w_half.xyz;
    float halfW    = obb.w_half.w;

    for (int su = -1; su <= 1; su += 2) {
    for (int sv = -1; sv <= 1; sv += 2) {
    for (int sw = -1; sw <= 1; sw += 2) {
        float3 localPos = center + axisU * halfU * float(su) + axisV * halfV * float(sv) + axisW * halfW * float(sw);
        float4 cp = mul(float4(localPos, 1.0), mvp);
        if (cp.w <= 0.0) continue;

        float3 ndcCorner = cp.xyz / cp.w;
        nearZ_OBB = min(nearZ_OBB, ndcCorner.z);
        minXY = min(minXY, ndcCorner.xy);
        maxXY = max(maxXY, ndcCorner.xy);
        cornerCount++;
    }}}

    if (cornerCount == 0) {
        cullEntries[NonUniformResourceIndex(block)][elemIndex].indexCount = 0;
        return;
    }

    float2 screenMin = minXY * 0.5 + 0.5;
    float2 screenMax = maxXY * 0.5 + 0.5;
    screenMin.y = 1.0 - screenMin.y;
    screenMax.y = 1.0 - screenMax.y;
    screenMin = clamp(screenMin, 0.0, 1.0);
    screenMax = clamp(screenMax, 0.0, 1.0);

    float2 aabbSize = screenMax - screenMin;
    float maxDim = max(aabbSize.x * float(pc.hizWidth), aabbSize.y * float(pc.hizHeight));
    int obbMip = int(floor(log2(max(maxDim, 1.0))));
    obbMip = min(obbMip, int(floor(log2(max(float(pc.hizWidth), float(pc.hizHeight))))));

    float2 centerUV = (screenMin + screenMax) * 0.5;
    float obbHizDepth = hizTex.SampleLevel(hizSampler, centerUV, float(obbMip)).r;
    if (nearZ_OBB > obbHizDepth + 0.001) {
        cullEntries[NonUniformResourceIndex(block)][elemIndex].indexCount = 0;
        return;
    }

    // refine: walk down the Hi-Z pyramid checking individual texels for conservative occlusion
    uint refineSteps = pc.refineLevel;
    while (refineSteps > 0 && obbMip > 0) {
        obbMip--;
        refineSteps--;

        uint dimX, dimY, dimL;
        hizTex.GetDimensions(uint(obbMip), dimX, dimY, dimL);
        int2 texelMin = int2(floor(screenMin * float2(int2(dimX, dimY))));
        int2 texelMax = int2(ceil(screenMax * float2(int2(dimX, dimY))));
        texelMin = max(texelMin, int2(0, 0));
        texelMax = min(texelMax, int2(dimX, dimY) - 1);

        int maxTexels = 64;
        int texelsTested = 0;

        for (int y = texelMin.y; y <= texelMax.y && texelsTested < maxTexels; y++) {
            for (int x = texelMin.x; x <= texelMax.x && texelsTested < maxTexels; x++, texelsTested++) {
                float finerDepth = hizTex.Load(int3(x, y, obbMip)).r;
                if (nearZ_OBB > finerDepth + 0.001) {
                    cullEntries[NonUniformResourceIndex(block)][elemIndex].indexCount = 0;
                    return;
                }
            }
        }
    }
}
