// Occluder selection + occluder-indirection compaction
// (docs/pre-prepass-occlusion-culling.md §5.1-A, §5.3).
//
// Runs once per submesh after expand. Each thread projects its submesh's OBB
// (model-space center + half-extent axes, transformed by the expand-written
// mvp) to a screen-pixel AABB and, when the coverage passes the area
// threshold, appends the submesh to the occluder set: its indirection range is
// copied into the occluder-only draw buffer and the CullEntry.flags bit is set
// so the pre-cull pass excludes it from the depth-prepass survivor list while
// collect keeps drawing it in the main pass.
//
// No sort, no scan, no ranking: any set of large real-geometry occluders is a
// valid occluder set — selection can only miss culls, never cause false ones.
#include "scene_entries.slang"

[[vk::binding(0, 0)]]
RWStructuredBuffer<CullEntry, CDataLayout> cullEntries[];

// Per-submesh transform constants written by expand (176 B VertexEntry).
[[vk::binding(1, 0)]]
StructuredBuffer<VertexEntry, CDataLayout> submeshVertexEntries[];

// Oriented bounding box: center + 3 half-extent axes (xyz = axis, w = half-length),
// model space (same convention as occlusion_cull.slang).
[[vk::binding(2, 0)]]
StructuredBuffer<ObbEntry, CDataLayout> obbEntries[];

// Per-technique flag table (SceneRenderer::UpdateTechniqueFlags). Bit layout
// must match TECHNIQUE_FLAG_* in SceneRenderer.cppm.
[[vk::binding(3, 0)]]
StructuredBuffer<uint, CDataLayout> techniqueFlags;

// Absolute entry slots, one per occurrence, produced by expand.
[[vk::binding(4, 0)]]
StructuredBuffer<uint, CDataLayout> drawIndices;

// Compaction destination, viewed as raw u32:
//   monolithic: a packed uint index list
//   MID:        20 B (5 x u32) VkDrawIndexedIndirectCommand records
[[vk::binding(5, 0)]]
RWStructuredBuffer<uint, CDataLayout> occluderOut;

// Word 0. Monolithic: accumulates the occluder index total. MID: increments
// once per emitted command. Zeroed by the CPU each frame.
[[vk::binding(6, 0)]]
globallycoherent RWStructuredBuffer<uint, CDataLayout> occluderOutCounter;

// Entry budget counter, zeroed by the CPU each frame.
[[vk::binding(7, 0)]]
RWStructuredBuffer<uint, CDataLayout> occluderCandidateCount;

#include "technique_packing.slang"

static const uint kMaxOccluders     = 512;
static const uint kMaxOccluderIndices = 262144; // one giant mesh cannot blow the budget

struct OccluderSelectPC {
    uint elementCount;
    uint minAreaPx; // acceptance threshold, screen pixels squared
    uint screenW;
    uint screenH;
};

[[vk::push_constant]]
ConstantBuffer<OccluderSelectPC> pc;

[numthreads(64, 1, 1)]
[shader("compute")]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint submeshIndex = dispatchThreadId.x;
    if (submeshIndex >= pc.elementCount) return;

    uint block = submeshIndex / 256u;
    uint elementIndex = submeshIndex % 256u;

    CullEntry entry = cullEntries[NonUniformResourceIndex(block)][elementIndex];
    if (entry.indexCount == 0) return;
    if (entry.indexCount > kMaxOccluderIndices) return;

    uint flags = techniqueFlags[entry.techniqueId];
    if ((flags & kFlagDepthPass) == 0) return;    // no prepass depth → never occludes
    if ((flags & kFlagOccluderSafe) == 0) return; // displaced bounds not conservative (§5.5)

    float4x4 mvp = submeshVertexEntries[NonUniformResourceIndex(block)][elementIndex].mvp;
    ObbEntry obb = obbEntries[NonUniformResourceIndex(block)][elementIndex];

    float3 center = obb.center_pad.xyz;
    float3 axisU = obb.u_half.xyz;
    float halfU = obb.u_half.w;
    float3 axisV = obb.v_half.xyz;
    float halfV = obb.v_half.w;
    float3 axisW = obb.w_half.xyz;
    float halfW = obb.w_half.w;

    // Screen-pixel AABB from the 8 transformed corners. Any clipped corner
    // (w <= 0) means the silhouette may reach the whole screen: select the
    // candidate unconditionally rather than estimate an area. Missing a big
    // occluder only misses culls; underestimating its area could too.
    bool cornerClipped = false;
    float2 minXY = float2(3.402823e+38, 3.402823e+38);
    float2 maxXY = float2(-3.402823e+38, -3.402823e+38);
    for (int su = -1; su <= 1; su += 2) {
    for (int sv = -1; sv <= 1; sv += 2) {
    for (int sw = -1; sw <= 1; sw += 2) {
        float3 localPos = center + axisU * halfU * float(su)
                                 + axisV * halfV * float(sv)
                                 + axisW * halfW * float(sw);
        float4 cp = mul(float4(localPos, 1.0), mvp);
        if (cp.w <= 0.0) {
            cornerClipped = true;
            continue;
        }
        float3 ndc = cp.xyz / cp.w;
        minXY = min(minXY, ndc.xy);
        maxXY = max(maxXY, ndc.xy);
    }}}

    float2 extentPx;
    if (cornerClipped) {
        extentPx = float2(float(pc.screenW), float(pc.screenH));
    } else {
        // NDC -> pixels with a y-flip (row 0 of the image is NDC +y). The flip
        // inverts the y order, so sort the flipped corners into a proper
        // min/max rect (same uvA/uvB convention as occlusion_cull).
        const float2 pA = float2(minXY.x * 0.5 + 0.5, (1.0 - maxXY.y) * 0.5) * float2(float(pc.screenW), float(pc.screenH));
        const float2 pB = float2(maxXY.x * 0.5 + 0.5, (1.0 - minXY.y) * 0.5) * float2(float(pc.screenW), float(pc.screenH));
        float2 lo = min(pA, pB);
        float2 hi = max(pA, pB);
        // Fully off-screen occluders occlude nothing visible.
        if (hi.x <= 0.0 || lo.x >= float(pc.screenW) || hi.y <= 0.0 || lo.y >= float(pc.screenH)) return;
        extentPx = hi - lo;
    }

    if (extentPx.x * extentPx.y < pc.minAreaPx) return;

    // Budgeted selection: no sort, no scan. Any set of large real-geometry
    // occluders is a valid occluder set — overflow only misses culls.
    uint slot;
    InterlockedAdd(occluderCandidateCount[0], 1u, slot);
    if (slot >= kMaxOccluders) return;

    if (kCompactionMode == 0) {
        // Monolithic: copy the 4 B entry slots into the packed index list.
        uint destOffset;
        InterlockedAdd(occluderOutCounter[0], entry.indexCount, destOffset);
        for (uint i = 0; i < entry.indexCount; i++) {
            occluderOut[destOffset + i] = drawIndices[entry.indexBase + i];
        }
    } else {
        // MID: one 20 B indexed draw command per selected occluder.
        uint cmdSlot;
        InterlockedAdd(occluderOutCounter[0], 1u, cmdSlot);
        uint base = cmdSlot * kCmdWords;
        occluderOut[base + 0] = entry.indexCount;
        occluderOut[base + 1] = 1u;
        occluderOut[base + 2] = entry.indexBase;
        occluderOut[base + 3] = 0u;
        // firstInstance carries the submesh id (see collect_count_compact.slang).
        occluderOut[base + 4] = submeshIndex;
    }

    cullEntries[NonUniformResourceIndex(block)][elementIndex].flags |= kOccluderBit;
}
