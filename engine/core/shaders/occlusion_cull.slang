// Per-submesh transform and material info (written by expand pass).
#include "scene_entries.slang"

[[vk::binding(0, 0)]]
StructuredBuffer<VertexEntry, CDataLayout> submeshVertexEntries[];

[[vk::binding(1, 0)]]
RWStructuredBuffer<CullEntry, CDataLayout> cullEntries[];

[[vk::binding(2, 0)]]
StructuredBuffer<float4, CDataLayout> boundingSpheres[];

// Hi-Z pyramid of the depth prepass. Depth convention: non-reversed [0,1]
// (0 = near, 1 = far) and every texel stores the FARTHEST depth in its footprint
// (max reduction in hiz_gen). An object is occluded iff every Hi-Z texel it covers
// is in front of (closer than) its near pole.
[[vk::binding(3, 0)]]
Sampler2D<float> hizTex;

[[vk::binding(4, 0)]]
StructuredBuffer<ObbEntry, CDataLayout> obbEntries[];

// Per-technique pass participation flags (written by the CPU from PipelineFlags).
// Bit layout must match SceneRenderer's TECHNIQUE_FLAG_* constants:
//   bit0 participates_in_depth_pass, bit1 receives_occlusion,
//   bit2 participates_in_collect,    bit3 bounds_conservative (reserved)
[[vk::binding(5, 0)]]
StructuredBuffer<uint, CDataLayout> techniqueFlags;

#include "technique_packing.slang"

static const int kMaxRefineTexels = 64;

struct OccPC {
    uint elementCount;
    uint refineLevel;
    uint hizWidth;
    uint hizHeight;
    float4 projInfo;   // .x = |projection x-scale|, .y = |projection y-scale|,
                       // .z = projection z-scale, .w = 1.0 if perspective else 0.0
};

[[vk::push_constant]]
ConstantBuffer<OccPC> pc;

#include "hiz_helpers.slang"

void CullSubmesh(uint block, uint elementIndex) {
    cullEntries[NonUniformResourceIndex(block)][elementIndex].indexCount = 0;
}

[numthreads(64, 1, 1)]
[shader("compute")]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint submeshIndex = dispatchThreadId.x;
    if (submeshIndex >= pc.elementCount) return;

    uint block = submeshIndex / 256u;
    uint elementIndex = submeshIndex % 256u;

    uint currentIndexCount = cullEntries[NonUniformResourceIndex(block)][elementIndex].indexCount;
    if (currentIndexCount == 0) return;

    // Techniques with receives_occlusion == false (e.g. transparents) are never
    // Hi-Z culled: kept alive unconditionally, regardless of test outcome.
    if ((techniqueFlags[cullEntries[NonUniformResourceIndex(block)][elementIndex].techniqueId]
         & kFlagReceivesOcclusion) == 0) return;

    float4x4 mvp = submeshVertexEntries[NonUniformResourceIndex(block)][elementIndex].mvp;
    float maxScaleVal = submeshVertexEntries[NonUniformResourceIndex(block)][elementIndex].maxScale;

    // ── Bounding sphere test: cheap conservative early-out ──
    float4 sphere = boundingSpheres[NonUniformResourceIndex(block)][elementIndex];
    float4 clipPos = mul(float4(sphere.xyz, 1.0), mvp);
    if (clipPos.w <= 0.0) return;   // sphere center behind the camera, but the sphere may
                                    // still be visible (camera inside it / near plane)

    float worldRadius = sphere.w * maxScaleVal;
    if (NearPoleClipped(clipPos, worldRadius, pc.projInfo)) return;   // intersects the near plane → keep alive
    float nearZ = NearPoleDepth(clipPos, worldRadius, pc.projInfo);

    // Screen-space AABB of the sphere. The radius is measured at the near-pole depth
    // (clipPos.w - worldRadius), not the center depth: the apparent size of a sphere
    // grows as it approaches the camera and the silhouette is an ellipse offset from
    // the projected center, so the cheaper "center depth" radius would under-estimate
    // the rect and falsely cull large close spheres near a screen edge. Using the
    // larger of the x/y NDC scales for both axes over-estimates further and keeps the
    // test conservative.
    float2 centerUV = clipPos.xy / clipPos.w * 0.5 + 0.5;
    centerUV.y = 1.0 - centerUV.y;
    float radiusUV = worldRadius * max(pc.projInfo.x, pc.projInfo.y)
                   / max(clipPos.w - worldRadius, kDepthEps) * 0.5;
    float2 minUV = centerUV - radiusUV;
    float2 maxUV = centerUV + radiusUV;
    // Fully off-screen iff the whole rect lies outside [0,1]^2. Testing the unclamped
    // bounds one-sided (instead of a clamped degenerate min/max) also keeps a
    // zero-area rect (e.g. an object with degenerate/uncomputed bounds) on-screen,
    // so it is never treated as off-screen while it is actually visible.
    if (maxUV.x <= 0.0 || minUV.x >= 1.0 || maxUV.y <= 0.0 || minUV.y >= 1.0) {
        CullSubmesh(block, elementIndex);
        return;
    }
    float2 clampedMin = clamp(minUV, 0.0, 1.0);
    float2 clampedMax = clamp(maxUV, 0.0, 1.0);

    int mip = RectMip(clampedMax - clampedMin, int(pc.hizWidth), int(pc.hizHeight));
    float hizDepthVal = hizTex.SampleLevel((clampedMin + clampedMax) * 0.5, float(mip));
    if (nearZ > hizDepthVal + kDepthEps) {
        CullSubmesh(block, elementIndex);
        return;
    }

    // ── OBB test: exact screen AABB from the transformed corners ──
    ObbEntry obb = obbEntries[NonUniformResourceIndex(block)][elementIndex];

    float3 center = obb.center_pad.xyz;
    float3 axisU = obb.u_half.xyz;
    float halfU = obb.u_half.w;
    float3 axisV = obb.v_half.xyz;
    float halfV = obb.v_half.w;
    float3 axisW = obb.w_half.xyz;
    float halfW = obb.w_half.w;

    // Conservative near depth for the OBB. The min over the 8 corners can be too far
    // (the nearest point of the box is often a face-center, nearer than every corner),
    // which makes the "nearZ > hizDepth" tests fire too aggressively and cull objects
    // that are still visible. Instead use the near pole of the bounding sphere of the
    // box: a sphere centered at the box center with radius sqrt(hu^2+hv^2+hw^2) scaled
    // by maxScale is guaranteed to contain the box (maxScale bounds every axis length
    // under the model transform), so its near pole is at least as close to the camera
    // as any surface point of the object and can never cause a false cull.
    float4 obbClipPos = mul(float4(center, 1.0), mvp);
    float obbWorldRadius = maxScaleVal * sqrt(halfU * halfU + halfV * halfV + halfW * halfW);
    if (NearPoleClipped(obbClipPos, obbWorldRadius, pc.projInfo)) return;
    float nearZ_OBB = NearPoleDepth(obbClipPos, obbWorldRadius, pc.projInfo);

    // Screen-space AABB of the OBB from its transformed corners. Because the sphere
    // guard above guarantees every corner is in front of the near plane, the loop
    // normally produces a complete AABB; the clipped-corner fallbacks below are kept
    // as defensive conservatism in case the bounds are ever imperfect.
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

    if (cornerCount == 0) return;   // all corners clipped: the box may surround the camera → keep alive
    if (cornerClipped) {
        // If ANY corner is clipped the silhouette would extend beyond the surviving
        // corners' AABB, so fall back to the full screen. That keeps the refinement's
        // "cull only if every texel is in front" test conservative: a too-small AABB
        // would let an object visible outside it get falsely culled.
        minXY = float2(-1.0, -1.0);
        maxXY = float2(1.0, 1.0);
    }

    // NDC -> UV with a y-flip (row 0 of the depth image is NDC +y). The flip
    // inverts the y order, so sort the two flipped corners into a proper
    // min/max rect; otherwise the refinement's texel loop iterates an empty
    // range and falsely culls everything.
    const float2 uvA = float2(minXY.x * 0.5 + 0.5, (1.0 - maxXY.y) * 0.5);
    const float2 uvB = float2(maxXY.x * 0.5 + 0.5, (1.0 - minXY.y) * 0.5);
    float2 screenMin = clamp(min(uvA, uvB), 0.0, 1.0);
    float2 screenMax = clamp(max(uvA, uvB), 0.0, 1.0);

    int obbMip = RectMip(screenMax - screenMin, int(pc.hizWidth), int(pc.hizHeight));

    float2 centerUV_OBB = (screenMin + screenMax) * 0.5;
    float obbHizDepth = hizTex.SampleLevel(centerUV_OBB, float(obbMip));
    if (nearZ_OBB > obbHizDepth + kDepthEps) {
        CullSubmesh(block, elementIndex);
        return;
    }

    // Refine: walk down the Hi-Z pyramid checking individual texels for conservative
    // occlusion. The object is only occluded if EVERY texel covered by its screen AABB
    // is in front of it; a single visible texel (e.g. the object peeking out beside an
    // occluder, or a texel showing the object's own depth) must keep it alive.
    uint refineSteps = pc.refineLevel;
    bool fullyOccluded = true;
    bool ranRefinement = false;
    while (refineSteps > 0 && obbMip > 0) {
        obbMip--;
        refineSteps--;
        ranRefinement = true;

        uint dimX, dimY, dimL;
        hizTex.GetDimensions(uint(obbMip), dimX, dimY, dimL);
        int2 texelMin = int2(floor(screenMin * float2(int2(dimX, dimY))));
        int2 texelMax = int2(ceil(screenMax * float2(int2(dimX, dimY))));
        texelMin = max(texelMin, int2(0, 0));
        texelMax = min(texelMax, int2(dimX, dimY) - 1);

        // If the AABB spans more texels than the cap, not every texel is checked; be
        // conservative and treat the object as visible rather than risk a false cull.
        bool capped = false;
        int texelsTested = 0;

        for (int y = texelMin.y; y <= texelMax.y && !capped; y++) {
            for (int x = texelMin.x; x <= texelMax.x; x++) {
                if (texelsTested >= kMaxRefineTexels) { capped = true; break; }
                texelsTested++;

                float finerDepth = hizTex.Load(int3(x, y, obbMip));
                // A texel is occluding if its depth is in front of the object's near
                // pole; a texel holding anything farther (the object itself, background,
                // or a gap) proves the object is visible there and must keep it alive.
                if (finerDepth > nearZ_OBB - kDepthEps) {
                    fullyOccluded = false;
                    break;
                }
            }
            if (!fullyOccluded) break;
        }

        if (capped) fullyOccluded = false;
        if (!fullyOccluded) break;
    }
    // The coarse single-texel test above returns early when it proves occlusion.
    // Reaching here means it did not, so cull only when a refinement actually ran
    // and every covered texel was occluding. With obbMip == 0 or refineLevel == 0
    // the loop cannot run and the object must be kept.
    if (ranRefinement && fullyOccluded) {
        CullSubmesh(block, elementIndex);
    }
}
