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
// Occluders (flags bit set by occluder_select) return before the tests: they
// already wrote their depth in the occluder prepass and must NOT be zeroed —
// collect keeps drawing them in the main pass.
//
// Depth convention: non-reversed [0,1] (0 = near, 1 = far), every Hi-Z texel
// stores the FARTHEST depth in its footprint (max reduction in hiz_gen).
#include "scene_entries.slang"

[[vk::binding(0, 0)]]
StructuredBuffer<VertexEntry, CDataLayout> submeshVertexEntries[];

[[vk::binding(1, 0)]]
RWStructuredBuffer<CullEntry, CDataLayout> cullEntries[];

[[vk::binding(2, 0)]]
StructuredBuffer<float4, CDataLayout> boundingSpheres[];

[[vk::binding(3, 0)]]
Sampler2D<float> hizTex;

[[vk::binding(4, 0)]]
StructuredBuffer<ObbEntry, CDataLayout> obbEntries[];

// Per-technique flag table (SceneRenderer::UpdateTechniqueFlags). Bit layout
// must match TECHNIQUE_FLAG_* in SceneRenderer.cppm:
//   bit0 participates_in_depth_pass, bit1 receives_occlusion,
//   bit2 participates_in_collect,    bit3 occluder-safe (conservative bounds)
[[vk::binding(5, 0)]]
StructuredBuffer<uint, CDataLayout> techniqueFlags;

// Absolute entry slots, one per occurrence, produced by expand.
[[vk::binding(6, 0)]]
StructuredBuffer<uint, CDataLayout> drawIndices;

// Compaction destination, viewed as raw u32:
//   monolithic: a packed uint index list
//   MID:        20 B (5 x u32) VkDrawIndexedIndirectCommand records
[[vk::binding(7, 0)]]
RWStructuredBuffer<uint, CDataLayout> depthOut;

// Counter word. Monolithic: word 0 accumulates the survivor index total.
// MID: increments once per emitted command. Zeroed by the CPU each frame.
[[vk::binding(8, 0)]]
globallycoherent RWStructuredBuffer<uint, CDataLayout> depthOutCounter;

// 0 = monolithic (compact index copy), 1 = MID (command emission).
#include "technique_packing.slang"
#include "hiz_helpers.slang"

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

[numthreads(64, 1, 1)]
[shader("compute")]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint submeshIndex = dispatchThreadId.x;
    if (submeshIndex >= pc.elementCount) return;

    uint block = submeshIndex / 256u;
    uint elementIndex = submeshIndex % 256u;

    CullEntry entry = cullEntries[NonUniformResourceIndex(block)][elementIndex];
    if (entry.indexCount == 0) return; // already culled earlier this frame

    // Occluders already wrote their depth in the occluder prepass; they are
    // visible and must reach the main pass through collect — never re-test
    // them and never compact them into the survivor list.
    if ((entry.flags & kOccluderBit) != 0) return;

    uint flags = techniqueFlags[entry.techniqueId];

    float4x4 mvp = submeshVertexEntries[NonUniformResourceIndex(block)][elementIndex].mvp;
    float maxScaleVal = submeshVertexEntries[NonUniformResourceIndex(block)][elementIndex].maxScale;

    // ── Bounding sphere frustum test: cheap conservative early-out ──
    float4 sphere = boundingSpheres[NonUniformResourceIndex(block)][elementIndex];
    float4 clipPos = mul(float4(sphere.xyz, 1.0), mvp);
    if (clipPos.w <= 0.0) return;   // sphere center behind the camera, but the
                                    // sphere may still be visible → keep alive

    float worldRadius = sphere.w * maxScaleVal;
    if (NearPoleClipped(clipPos, worldRadius, pc.projInfo)) return;   // intersects near plane

    float2 centerUV = clipPos.xy / clipPos.w * 0.5 + 0.5;
    centerUV.y = 1.0 - centerUV.y;
    float radiusUV = worldRadius * max(pc.projInfo.x, pc.projInfo.y)
                   / max(clipPos.w - worldRadius, kDepthEps) * 0.5;
    float2 minUV = centerUV - radiusUV;
    float2 maxUV = centerUV + radiusUV;
    // One-sided unclamped off-screen test (never treats a zero-area rect as
    // off-screen) — see occlusion_cull.slang for the full rationale.
    if (maxUV.x <= 0.0 || minUV.x >= 1.0 || maxUV.y <= 0.0 || minUV.y >= 1.0) {
        cullEntries[NonUniformResourceIndex(block)][elementIndex].indexCount = 0;
        return;
    }

    // ── Hi-Z occlusion test (only techniques that receive occlusion) ──
    if ((flags & kFlagReceivesOcclusion) != 0) {
        float nearZ = NearPoleDepth(clipPos, worldRadius, pc.projInfo);
        float2 clampedMin = clamp(minUV, 0.0, 1.0);
        float2 clampedMax = clamp(maxUV, 0.0, 1.0);

        int mip = RectMip(clampedMax - clampedMin, int(pc.hizWidth), int(pc.hizHeight));
        float hizDepthVal = hizTex.SampleLevel((clampedMin + clampedMax) * 0.5, float(mip));
        if (nearZ > hizDepthVal + kDepthEps) {
            cullEntries[NonUniformResourceIndex(block)][elementIndex].indexCount = 0;
            return;
        }

        // OBB test: exact screen AABB from the transformed corners, with the
        // conservative sphere-based near bound (the min-over-corners variant is
        // a documented false-cull hazard — see occlusion_cull.slang).
        ObbEntry obb = obbEntries[NonUniformResourceIndex(block)][elementIndex];


        float3 center = obb.center_pad.xyz;
        float3 axisU = obb.u_half.xyz;
        float halfU = obb.u_half.w;
        float3 axisV = obb.v_half.xyz;
        float halfV = obb.v_half.w;
        float3 axisW = obb.w_half.xyz;
        float halfW = obb.w_half.w;

        float4 obbClipPos = mul(float4(center, 1.0), mvp);
        float obbWorldRadius = maxScaleVal * sqrt(halfU * halfU + halfV * halfV + halfW * halfW);
        if (NearPoleClipped(obbClipPos, obbWorldRadius, pc.projInfo)) {
            // near-pole clipped → cannot test conservatively, keep alive
        } else {
            float nearZ_OBB = NearPoleDepth(obbClipPos, obbWorldRadius, pc.projInfo);

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

                int obbMip = RectMip(screenMax - screenMin, int(pc.hizWidth), int(pc.hizHeight));
                float2 centerUV_OBB = (screenMin + screenMax) * 0.5;
                float obbHizDepth = hizTex.SampleLevel(centerUV_OBB, float(obbMip));
                if (nearZ_OBB > obbHizDepth + kDepthEps) {
                    cullEntries[NonUniformResourceIndex(block)][elementIndex].indexCount = 0;
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

    if (kCompactionMode == 0) {
        // Monolithic: copy the 4 B entry slots into the packed index list.
        uint destOffset;
        InterlockedAdd(depthOutCounter[0], entry.indexCount, destOffset);
        for (uint i = 0; i < entry.indexCount; i++) {
            depthOut[destOffset + i] = drawIndices[entry.indexBase + i];
        }
    } else {
        // MID: one 20 B indexed draw command per survivor.
        uint slot;
        InterlockedAdd(depthOutCounter[0], 1u, slot);
        uint base = slot * kCmdWords;
        depthOut[base + 0] = entry.indexCount;
        depthOut[base + 1] = 1u;
        depthOut[base + 2] = entry.indexBase;
        depthOut[base + 3] = 0u;
        depthOut[base + 4] = 0u;
    }
}
