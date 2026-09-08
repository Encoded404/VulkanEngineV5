// Pre-cull: frustum reject + Hi-Z occlusion test + depth-prepass survivor
// compaction (docs/pre-prepass-occlusion-culling.md §5.1-D, §5.3).
//
// Runs after the occluder prepass and its Hi-Z build, before the full depth
// prepass. Per submesh:
//   1. conservative frustum reject (bounding-sphere screen AABB),
//   2. Hi-Z occlusion test against the occluder-prepass pyramid,
//   3. culled submeshes get cullEntries.indexCount = 0 (the existing
//      post-prepass occlusion pass and collect skip them for free),
//   4. surviving depth-pass participants are compacted into the
//      depth-prepass-only indirection buffer + draw command.
//
// Occluders (pad bit set by occluder_select) return before the tests: they
// already wrote their depth in the occluder prepass and must NOT be zeroed —
// collect keeps drawing them in the main pass.
//
// Depth convention: non-reversed [0,1] (0 = near, 1 = far), every Hi-Z texel
// stores the FARTHEST depth in its footprint (max reduction in hiz_gen).
struct VertEntry {
    float4x4 MVP;
    float maxScale;
    uint materialId;
    uint ormPacked;
    float4x4 modelMatrix;
    float3x3 normalMatrix;
};

[[vk::binding(0, 0)]]
StructuredBuffer<VertEntry, CDataLayout> submeshVertexInfo[];

struct CullEntry {
    uint indirOffset;
    uint indexCount;
    uint techniqueId;
    uint pad; // bit0 = selected occluder (set by occluder_select)
};

[[vk::binding(1, 0)]]
RWStructuredBuffer<CullEntry, CDataLayout> cullEntries[];

[[vk::binding(2, 0)]]
StructuredBuffer<float4, CDataLayout> boundingSpheres[];

[[vk::binding(3, 0)]]
Sampler2D<float> hizTex;

struct OBBGPU {
    float4 center_pad;
    float4 u_half;
    float4 v_half;
    float4 w_half;
};

[[vk::binding(4, 0)]]
StructuredBuffer<OBBGPU, CDataLayout> obbBuf[];

// Per-technique flag table (SceneRenderer::UpdateTechniqueFlags). Bit layout
// must match TECHNIQUE_FLAG_* in SceneRenderer.cppm:
//   bit0 participates_in_depth_pass, bit1 receives_occlusion,
//   bit2 participates_in_collect,    bit3 occluder-safe (conservative bounds)
[[vk::binding(5, 0)]]
StructuredBuffer<uint, CDataLayout> techniqueFlags;

struct IndirEntry {
    uint vertexId;
    uint submeshId;
};

[[vk::binding(6, 0)]]
StructuredBuffer<IndirEntry, CDataLayout> fullIndirect;

[[vk::binding(7, 0)]]
RWStructuredBuffer<IndirEntry, CDataLayout> prepassIndirect;

// DrawIndirectCommand buffer pre-initialized by the CPU to {0, 1, 0, 0}; word 0
// (vertexCount) accumulates the survivor index total via InterlockedAdd.
[[vk::binding(8, 0)]]
globallycoherent RWStructuredBuffer<uint, CDataLayout> prepassCounter;

static const uint kFlagDepthPass          = 1u << 0;
static const uint kFlagReceivesOcclusion  = 1u << 1;
static const uint kOccluderBit            = 1u << 0;

static const float kDepthEps = 0.001;

struct PreCullPC {
    uint elementCount;
    uint hizWidth;
    uint hizHeight;
    uint pad0;
    float4 projInfo; // .x = |projection x-scale|, .y = |projection y-scale|,
                     // .z = projection z-scale, .w = 1.0 if perspective else 0.0
};

[[vk::push_constant]]
ConstantBuffer<PreCullPC> pc;

// Largest valid Hi-Z mip index (hiz level 0 is half the depth resolution).
int MaxHizMip() {
    return int(floor(log2(max(float(pc.hizWidth), float(pc.hizHeight)))));
}

// Mip whose texels are ~1-2 level-0 texels large, so a single sample at that
// mip covers the whole rect (rectSizeUV in [0,1] UV units).
int RectMip(float2 rectSizeUV) {
    float maxDim = max(rectSizeUV.x * float(pc.hizWidth), rectSizeUV.y * float(pc.hizHeight));
    return min(int(ceil(log2(max(maxDim, 1.0)))), MaxHizMip());
}

// Depth (NDC z) of the near pole of a sphere of radius worldR whose center
// projects to clipPos. Ported from occlusion_cull.slang: exact for rolled
// views and scaled models, correct for perspective and orthographic.
float NearPoleDepth(float4 clipPos, float worldR) {
    if (pc.projInfo.w > 0.5)
        return (clipPos.z + pc.projInfo.z * worldR) / max(clipPos.w - worldR, kDepthEps);
    return clipPos.z + pc.projInfo.z * worldR;
}

// True when the near pole is at/beyond the near plane: the object is at least
// partially visible and must never be culled. Ported from occlusion_cull.slang.
bool NearPoleClipped(float4 clipPos, float worldR) {
    return clipPos.z + pc.projInfo.z * worldR <= kDepthEps;
}

[numthreads(64, 1, 1)]
[shader("compute")]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint id = dispatchThreadId.x;
    if (id >= pc.elementCount) return;

    uint block = id / 256u;
    uint elemIndex = id % 256u;

    CullEntry entry = cullEntries[NonUniformResourceIndex(block)][elemIndex];
    if (entry.indexCount == 0) return; // already culled earlier this frame

    // Occluders already wrote their depth in the occluder prepass; they are
    // visible and must reach the main pass through collect — never re-test
    // them and never compact them into the survivor list.
    if ((entry.pad & kOccluderBit) != 0) return;

    uint flags = techniqueFlags[entry.techniqueId];

    float4x4 mvp = submeshVertexInfo[NonUniformResourceIndex(block)][elemIndex].MVP;
    float maxScaleVal = submeshVertexInfo[NonUniformResourceIndex(block)][elemIndex].maxScale;

    // ── Bounding sphere frustum test: cheap conservative early-out ──
    float4 sphere = boundingSpheres[NonUniformResourceIndex(block)][elemIndex];
    float4 clipPos = mul(float4(sphere.xyz, 1.0), mvp);
    if (clipPos.w <= 0.0) return;   // sphere center behind the camera, but the
                                    // sphere may still be visible → keep alive

    float worldRadius = sphere.w * maxScaleVal;
    if (NearPoleClipped(clipPos, worldRadius)) return;   // intersects near plane

    float2 centerUV = clipPos.xy / clipPos.w * 0.5 + 0.5;
    centerUV.y = 1.0 - centerUV.y;
    float radiusUV = worldRadius * max(pc.projInfo.x, pc.projInfo.y)
                   / max(clipPos.w - worldRadius, kDepthEps) * 0.5;
    float2 minUV = centerUV - radiusUV;
    float2 maxUV = centerUV + radiusUV;
    // One-sided unclamped off-screen test (never treats a zero-area rect as
    // off-screen) — see occlusion_cull.slang for the full rationale.
    if (maxUV.x <= 0.0 || minUV.x >= 1.0 || maxUV.y <= 0.0 || minUV.y >= 1.0) {
        cullEntries[NonUniformResourceIndex(block)][elemIndex].indexCount = 0;
        return;
    }

    // ── Hi-Z occlusion test (only techniques that receive occlusion) ──
    if ((flags & kFlagReceivesOcclusion) != 0) {
        float nearZ = NearPoleDepth(clipPos, worldRadius);
        float2 clampedMin = clamp(minUV, 0.0, 1.0);
        float2 clampedMax = clamp(maxUV, 0.0, 1.0);

        int mip = RectMip(clampedMax - clampedMin);
        float hizDepthVal = hizTex.SampleLevel((clampedMin + clampedMax) * 0.5, float(mip));
        if (nearZ > hizDepthVal + kDepthEps) {
            cullEntries[NonUniformResourceIndex(block)][elemIndex].indexCount = 0;
            return;
        }

        // OBB test: exact screen AABB from the transformed corners, with the
        // conservative sphere-based near bound (the min-over-corners variant is
        // a documented false-cull hazard — see occlusion_cull.slang).
        OBBGPU obb = obbBuf[NonUniformResourceIndex(block)][elemIndex];

        float3 center = obb.center_pad.xyz;
        float3 axisU = obb.u_half.xyz;
        float halfU = obb.u_half.w;
        float3 axisV = obb.v_half.xyz;
        float halfV = obb.v_half.w;
        float3 axisW = obb.w_half.xyz;
        float halfW = obb.w_half.w;

        float4 obbClipPos = mul(float4(center, 1.0), mvp);
        float obbWorldRadius = maxScaleVal * sqrt(halfU * halfU + halfV * halfV + halfW * halfW);
        if (NearPoleClipped(obbClipPos, obbWorldRadius)) {
            // near-pole clipped → cannot test conservatively, keep alive
        } else {
            float nearZ_OBB = NearPoleDepth(obbClipPos, obbWorldRadius);

            int cornerCount = 0;
            bool cornerClipped = false;
            float2 minXY = float2(3.402823e+38, 3.402823e+38);
            float2 maxXY = float2(-3.402823e+38, -3.402823e+38);
            for (int su = -1; su <= 1; su += 2) {
            for (int sv = -1; sv <= 1; sv += 2) {
            for (int sw = -1; sw <= 1; sw += 2) {
                float3 localPos = center + axisU * halfU * float(su) + axisV * halfV * float(sv) + axisW * halfW * float(sw);
                float4 cp = mul(float4(localPos, 1.0), mvp);
                if (cp.w <= 0.0) {
                    cornerClipped = true;
                    continue;
                }
                float3 ndcCorner = cp.xyz / cp.w;
                minXY = min(minXY, ndcCorner.xy);
                maxXY = max(maxXY, ndcCorner.xy);
                cornerCount++;
            }}}

            if (cornerCount > 0) {
                if (cornerClipped) {
                    minXY = float2(-1.0, -1.0);
                    maxXY = float2(1.0, 1.0);
                }
                const float2 uvA = float2(minXY.x * 0.5 + 0.5, (1.0 - maxXY.y) * 0.5);
                const float2 uvB = float2(maxXY.x * 0.5 + 0.5, (1.0 - minXY.y) * 0.5);
                float2 screenMin = clamp(min(uvA, uvB), 0.0, 1.0);
                float2 screenMax = clamp(max(uvA, uvB), 0.0, 1.0);

                int obbMip = RectMip(screenMax - screenMin);
                float2 centerUV_OBB = (screenMin + screenMax) * 0.5;
                float obbHizDepth = hizTex.SampleLevel(centerUV_OBB, float(obbMip));
                if (nearZ_OBB > obbHizDepth + kDepthEps) {
                    cullEntries[NonUniformResourceIndex(block)][elemIndex].indexCount = 0;
                    return;
                }
            }
        }
    }

    // ── Survivor compaction into the depth-prepass indirection ──
    // Techniques with participates_in_depth_pass == false keep their
    // indexCount (collect still emits them for the main pass) but are not
    // part of the depth prepass.
    if ((flags & kFlagDepthPass) == 0) return;

    uint destOffset;
    InterlockedAdd(prepassCounter[0], entry.indexCount, destOffset);
    for (uint i = 0; i < entry.indexCount; i++) {
        prepassIndirect[destOffset + i] = fullIndirect[entry.indirOffset + i];
    }
}
