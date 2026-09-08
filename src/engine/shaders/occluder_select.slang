// Occluder selection + occluder-indirection compaction
// (docs/pre-prepass-occlusion-culling.md §5.1-A, §5.3).
//
// Runs once per submesh after expand. Each thread projects its submesh's OBB
// (model-space center + half-extent axes, transformed by the expand-written
// MVP) to a screen-pixel AABB and, when the coverage passes the area
// threshold, appends the submesh to the occluder set: its indirection range is
// copied into the occluder-only draw buffer and the CullEntry.pad bit is set
// so the pre-cull pass excludes it from the depth-prepass survivor list while
// collect keeps drawing it in the main pass.
//
// No sort, no scan, no ranking: any set of large real-geometry occluders is a
// valid occluder set — selection can only miss culls, never cause false ones.
struct CullEntry {
    uint indirOffset;
    uint indexCount;
    uint techniqueId;
    uint pad; // bit0 = selected occluder this frame
};

[[vk::binding(0, 0)]]
RWStructuredBuffer<CullEntry, CDataLayout> cull[];

// Per-submesh transform constants written by expand (176 B VertEntry).
struct VertEntry {
    float4x4 MVP;
    float maxScale;
    uint materialId;
    uint ormPacked;
    float4x4 modelMatrix;
    float3x3 normalMatrix;
};

[[vk::binding(1, 0)]]
StructuredBuffer<VertEntry, CDataLayout> submeshInfo[];

// Oriented bounding box: center + 3 half-extent axes (xyz = axis, w = half-length),
// model space (same convention as occlusion_cull.slang).
struct OBBGPU {
    float4 center_pad;
    float4 u_half;
    float4 v_half;
    float4 w_half;
};

[[vk::binding(2, 0)]]
StructuredBuffer<OBBGPU, CDataLayout> obbBuf[];

// Per-technique flag table (SceneRenderer::UpdateTechniqueFlags). Bit layout
// must match TECHNIQUE_FLAG_* in SceneRenderer.cppm.
[[vk::binding(3, 0)]]
StructuredBuffer<uint, CDataLayout> techniqueFlags;

struct IndirEntry {
    uint vertexId;
    uint submeshId;
};

[[vk::binding(4, 0)]]
StructuredBuffer<IndirEntry, CDataLayout> fullIndirect;

[[vk::binding(5, 0)]]
RWStructuredBuffer<IndirEntry, CDataLayout> occluderIndirect;

// DrawIndirectCommand buffer pre-initialized by the CPU to {0, 1, 0, 0}; word 0
// (vertexCount) accumulates the occluder index total via InterlockedAdd.
[[vk::binding(6, 0)]]
globallycoherent RWStructuredBuffer<uint, CDataLayout> occluderCounter;

// Entry budget counter, zeroed by the CPU each frame.
[[vk::binding(7, 0)]]
RWStructuredBuffer<uint, CDataLayout> occluderCount;

static const uint kFlagDepthPass    = 1u << 0;
static const uint kFlagOccluderSafe = 1u << 3;
static const uint kOccluderBit      = 1u << 0;
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
    uint id = dispatchThreadId.x;
    if (id >= pc.elementCount) return;

    uint block = id / 256u;
    uint elemIndex = id % 256u;

    CullEntry entry = cull[NonUniformResourceIndex(block)][elemIndex];
    if (entry.indexCount == 0) return;
    if (entry.indexCount > kMaxOccluderIndices) return;

    uint flags = techniqueFlags[entry.techniqueId];
    if ((flags & kFlagDepthPass) == 0) return;    // no prepass depth → never occludes
    if ((flags & kFlagOccluderSafe) == 0) return; // displaced bounds not conservative (§5.5)

    float4x4 mvp = submeshInfo[NonUniformResourceIndex(block)][elemIndex].MVP;
    OBBGPU obb = obbBuf[NonUniformResourceIndex(block)][elemIndex];

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
    InterlockedAdd(occluderCount[0], 1u, slot);
    if (slot >= kMaxOccluders) return;

    uint destOffset;
    InterlockedAdd(occluderCounter[0], entry.indexCount, destOffset);
    for (uint i = 0; i < entry.indexCount; i++) {
        occluderIndirect[destOffset + i] = fullIndirect[entry.indirOffset + i];
    }

    cull[NonUniformResourceIndex(block)][elemIndex].pad |= kOccluderBit;
}
