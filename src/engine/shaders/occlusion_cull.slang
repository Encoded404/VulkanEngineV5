struct VertEntry {
    float4x4 MVP;
    float maxScale;
    uint textureSlot;
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
RWStructuredBuffer<CullEntry> cull[];

[[vk::binding(2, 0)]]
StructuredBuffer<float4> sph[];

[[vk::binding(3, 0)]]
Texture2D<float> hizTex;
[[vk::binding(3, 0)]]
SamplerState hizSampler;

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
    uint elemIdx = id % 256u;

    uint cur = cull[NonUniformResourceIndex(block)][elemIdx].indexCount;
    if (cur == 0) return;

    float4x4 mvp = submeshVertexInfo[NonUniformResourceIndex(block)][elemIdx].MVP;
    float maxScaleVal = submeshVertexInfo[NonUniformResourceIndex(block)][elemIdx].maxScale;

    float4 sphereData = sph[NonUniformResourceIndex(block)][elemIdx];
    float3 sphereCenter = sphereData.xyz;
    float localRadius = sphereData.w;

    float4 clipPos = mul(float4(sphereCenter, 1.0), mvp);
    if (clipPos.w <= 0.0) {
        cull[NonUniformResourceIndex(block)][elemIdx].indexCount = 0;
        return;
    }

    float R = localRadius * maxScaleVal;

    float frontW = max(clipPos.w - R, 0.001);
    float nearZ = (clipPos.z - R) / frontW;

    float screenRadius = abs(R * mvp[1][1] / clipPos.w);
    int mip = int(floor(log2(max(screenRadius, 1.0))));
    mip = min(mip, int(floor(log2(max(float(pc.hizWidth), float(pc.hizHeight))))));

    float3 ndc = clipPos.xyz / clipPos.w;
    float2 screenPos = ndc.xy * 0.5 + 0.5;
    screenPos.y = 1.0 - screenPos.y;

    float hizDepthVal = hizTex.SampleLevel(hizSampler, screenPos, float(mip)).r;
    if (nearZ > hizDepthVal + 0.001) {
        cull[NonUniformResourceIndex(block)][elemIdx].indexCount = 0;
        return;
    }

    OBBGPU obb = obbBuf[NonUniformResourceIndex(block)][elemIdx];

    int cornerCount = 0;
    float nearZ_OBB = 3.402823e+38;
    float2 minXY = float2(3.402823e+38, 3.402823e+38);
    float2 maxXY = float2(-3.402823e+38, -3.402823e+38);

    float3 c  = obb.center_pad.xyz;
    float3 u  = obb.u_half.xyz;
    float hu = obb.u_half.w;
    float3 v  = obb.v_half.xyz;
    float hv = obb.v_half.w;
    float3 w  = obb.w_half.xyz;
    float hw = obb.w_half.w;

    for (int su = -1; su <= 1; su += 2) {
    for (int sv = -1; sv <= 1; sv += 2) {
    for (int sw = -1; sw <= 1; sw += 2) {
        float3 localPos = c + u * hu * float(su) + v * hv * float(sv) + w * hw * float(sw);
        float4 cp = mul(float4(localPos, 1.0), mvp);
        if (cp.w <= 0.0) continue;

        float3 ndcCorner = cp.xyz / cp.w;
        nearZ_OBB = min(nearZ_OBB, ndcCorner.z);
        minXY = min(minXY, ndcCorner.xy);
        maxXY = max(maxXY, ndcCorner.xy);
        cornerCount++;
    }}}

    if (cornerCount == 0) {
        cull[NonUniformResourceIndex(block)][elemIdx].indexCount = 0;
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
        cull[NonUniformResourceIndex(block)][elemIdx].indexCount = 0;
        return;
    }

    uint refine = pc.refineLevel;
    while (refine > 0 && obbMip > 0) {
        obbMip--;
        refine--;

        uint dimX, dimY, dimL;
        hizTex.GetDimensions(uint(obbMip), dimX, dimY, dimL);
        int2 tMin = int2(floor(screenMin * float2(int2(dimX, dimY))));
        int2 tMax = int2(ceil(screenMax * float2(int2(dimX, dimY))));
        tMin = max(tMin, int2(0, 0));
        tMax = min(tMax, int2(dimX, dimY) - 1);

        int maxTexels = 64;
        int tested = 0;

        for (int y = tMin.y; y <= tMax.y && tested < maxTexels; y++) {
            for (int x = tMin.x; x <= tMax.x && tested < maxTexels; x++, tested++) {
                float finerDepth = hizTex.Load(int3(x, y, obbMip)).r;
                if (nearZ_OBB > finerDepth + 0.001) {
                    cull[NonUniformResourceIndex(block)][elemIdx].indexCount = 0;
                    return;
                }
            }
        }
    }
}