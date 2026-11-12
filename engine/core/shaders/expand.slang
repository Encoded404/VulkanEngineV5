#include "scene_entries.slang"
#include "scene_block_layout.slang"
#include "technique_packing.slang"
#include "skin_layout.slang"

[[vk::binding(0, 0)]]
StructuredBuffer<DynamicEntry, CDataLayout> dynamicEntries[];

[[vk::binding(1, 0)]]
StructuredBuffer<StaticEntry, CDataLayout> staticEntries[];

[[vk::binding(2, 0)]]
RWStructuredBuffer<VertexEntry, CDataLayout> submeshVertexEntries[];

[[vk::binding(3, 0)]]
RWStructuredBuffer<CullEntry, CDataLayout> cullEntries[];

// One entry per vertex slot in each submesh's tight window. CID-only:
// there SV_VertexID is the draw_indices value, which is a slot here. MID reads
// the vertex buffer slot from VertexEntry and takes the vertex index from the
// raw builtin, so it never touches this buffer.
[[vk::binding(4, 0)]]
RWStructuredBuffer<VertexIndirectionEntry, CDataLayout> vertexIndirection;

// CID: absolute slot indices into vertexIndirection, one per index
// occurrence. MID: absolute vertex indices (baseVertex + rawIndex), one per
// occurrence, consumed directly as SV_VertexID.
[[vk::binding(5, 0)]]
RWStructuredBuffer<uint, CDataLayout> drawIndices;

// [0] = next vertex-indirection base, [1] = next draw-index base. Zeroed by the
// CPU. [2] is the per-frame deform-region allocator (element offset into the
// deformed-vertex buffer), zeroed with the others.
[[vk::binding(6, 0)]]
globallycoherent RWStructuredBuffer<uint, CDataLayout> expandCounter;

// Per-submesh deform addressing, written by gather. Expand is the only writer
// of `deform_base` (it owns the per-frame region allocator) and it also clears
// kDeformActive when the region budget cannot cover the submesh. Read only when
// `hasActiveDeform != 0`, so an unskinned scene touches none of it.
[[vk::binding(7, 0)]]
RWStructuredBuffer<SkinEntry, CDataLayout> skinEntries[];

struct ExpandPC {
    float4x4 vp;
    uint objectCount;
    uint pad0;
    uint pad1;
    // 0 when the frame has no deform dispatch: the skin-entry descriptor is
    // not read at all and every deformBias is the sentinel.
    uint hasActiveDeform;
    // Elements per deform region (T, not 2T): expand allocates only into A.
    uint regionSpan;
};

[[vk::push_constant]]
ConstantBuffer<ExpandPC> pushConstant;

[[vk::binding(0, 1)]]
StructuredBuffer<uint, CDataLayout> indexBuffers[];

float4x4 modelMat(float3 pos, float4 rot, float3 scale) {
    float x = rot.x, y = rot.y, z = rot.z, w = rot.w;
    float xx = x * x, yy = y * y, zz = z * z;
    float xy = x * y, xz = x * z, xw = x * w;
    float yz = y * z, yw = y * w, zw = z * w;

    return float4x4(
        (1.0 - 2.0 * (yy + zz)) * scale.x, 2.0 * (xy + zw) * scale.x,      2.0 * (xz - yw) * scale.x,      0.0,
        2.0 * (xy - zw) * scale.y,          (1.0 - 2.0 * (xx + zz)) * scale.y, 2.0 * (yz + xw) * scale.y,      0.0,
        2.0 * (xz + yw) * scale.z,           2.0 * (yz - xw) * scale.z,        (1.0 - 2.0 * (xx + yy)) * scale.z, 0.0,
        pos.x,                             pos.y,                          pos.z,                         1.0
    );
}

// Inverse-transpose of the 3x3 part of modelMat(pos, rot, scale).
// The effective model transform is M = R * S (R orthonormal from the
// quaternion, S diagonal), applied via mul(v, modelMatrix) — note modelMat
// stores the transpose of the math matrix, matching that row-vector usage.
// The normal matrix to apply the same way is (M^-1)^T = R * S^-1, which in
// this stored layout is exactly modelMat's 3x3 pattern with `scale`
// substituted by 1/scale per row. No matrix inversion needed.
// Guards against degenerate (zero-component) scales with 1e-8.
float3x3 normalMat(float4 rot, float3 scale) {
    float x = rot.x, y = rot.y, z = rot.z, w = rot.w;
    float xx = x * x, yy = y * y, zz = z * z;
    float xy = x * y, xz = x * z, xw = x * w;
    float yz = y * z, yw = y * w, zw = z * w;

    float3 invS = 1.0 / max(abs(scale), float3(1e-8, 1e-8, 1e-8));

    // Same element layout as modelMat(), invS per row where scale was.
    return float3x3(
        (1.0 - 2.0 * (yy + zz)) * invS.x, 2.0 * (xy + zw) * invS.x, 2.0 * (xz - yw) * invS.x,
        2.0 * (xy - zw) * invS.y,         (1.0 - 2.0 * (xx + zz)) * invS.y, 2.0 * (yz + xw) * invS.y,
        2.0 * (xz + yw) * invS.z,         2.0 * (yz - xw) * invS.z,         (1.0 - 2.0 * (xx + yy)) * invS.z
    );
}

[numthreads(64, 1, 1)]
[shader("compute")]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint submeshIndex = dispatchThreadId.x;
    if (submeshIndex >= pushConstant.objectCount) return;

    uint block = submeshIndex / kEntriesPerBlock;    // which storage buffer block (kEntriesPerBlock submeshes per block)
    uint elementIndex = submeshIndex % kEntriesPerBlock; // index within that block

    float3 position = dynamicEntries[NonUniformResourceIndex(block)][elementIndex].pos;
    float3 scale = dynamicEntries[NonUniformResourceIndex(block)][elementIndex].scale;
    float4 rotation = dynamicEntries[NonUniformResourceIndex(block)][elementIndex].rot;

    uint indexRange = staticEntries[NonUniformResourceIndex(block)][elementIndex].indexRange;
    uint techniqueMaterial = staticEntries[NonUniformResourceIndex(block)][elementIndex].techniqueMaterial;
    uint techId = techniqueMaterial & kTechniqueMask;
    uint matId  = techniqueMaterial >> kTechniqueBits;

    float maxScaleVal = max(abs(scale.x), max(abs(scale.y), abs(scale.z)));
    float4x4 model = modelMat(position, rotation, scale);
    float4x4 mvp = mul(model, pushConstant.vp);
    float3x3 nrm = normalMat(rotation, scale);

    submeshVertexEntries[NonUniformResourceIndex(block)][elementIndex].mvp = mvp;
    submeshVertexEntries[NonUniformResourceIndex(block)][elementIndex].maxScale = maxScaleVal;
    submeshVertexEntries[NonUniformResourceIndex(block)][elementIndex].materialId = matId;
    submeshVertexEntries[NonUniformResourceIndex(block)][elementIndex].ormPacked =
        staticEntries[NonUniformResourceIndex(block)][elementIndex].ormPacked;
    submeshVertexEntries[NonUniformResourceIndex(block)][elementIndex].modelMatrix = model;
    submeshVertexEntries[NonUniformResourceIndex(block)][elementIndex].normalMatrix = nrm;

    uint indexBufSlot = staticEntries[NonUniformResourceIndex(block)][elementIndex].indexStartPacked >> 24;
    uint indexOffset  = staticEntries[NonUniformResourceIndex(block)][elementIndex].indexStartPacked & 0xFFFFFFu;
    uint vertexBufferSlot = staticEntries[NonUniformResourceIndex(block)][elementIndex].vertexInfoPacked >> 24;
    uint baseVertex    = staticEntries[NonUniformResourceIndex(block)][elementIndex].vertexInfoPacked & 0xFFFFFFu;
    uint windowBase    = staticEntries[NonUniformResourceIndex(block)][elementIndex].vertexWindowBase;
    uint vertexSpan    = staticEntries[NonUniformResourceIndex(block)][elementIndex].vertexSpan;

    // Allocate this submesh's vertex-entry range and its draw-index range. The
    // ranges are independent: entry slots are dense over the tight vertex
    // window, index slots are dense over the index range. MID skips the
    // per-slot indirection stamping; the counter is still advanced so both
    // modes allocate identically.
    uint entryBase;
    InterlockedAdd(expandCounter[0], vertexSpan, entryBase);
    uint indexBase;
    InterlockedAdd(expandCounter[1], indexRange, indexBase);

    // Both modes publish the vertex buffer slot for the MID vertex shaders,
    // and the out-of-line UV1 addressing (block + signed index re-base) for the
    // main pass. kNoUvSlot means the set is absent and the shader emits UV0.
    submeshVertexEntries[NonUniformResourceIndex(block)][elementIndex].vertexBufferSlot = vertexBufferSlot;
    submeshVertexEntries[NonUniformResourceIndex(block)][elementIndex].uvSlot =
        staticEntries[NonUniformResourceIndex(block)][elementIndex].uvSlot;
    submeshVertexEntries[NonUniformResourceIndex(block)][elementIndex].uvBias =
        staticEntries[NonUniformResourceIndex(block)][elementIndex].uvBias;

    // ── Deform allocation ──
    // The GPU, not the CPU, owns each submesh's region base: the count pass
    // cannot know it before expand runs. A submesh whose region would run past
    // the frame's budget loses kDeformActive and keeps the sentinel bias, so it
    // renders its bind pose instead of corrupting another submesh's region.
    // When the frame has no deform at all, none of this touches the skin-entry
    // descriptor: an unskinned scene pays nothing but the branch.
    int deformBias = kNoDeform;
    uint cullFlags = 0;
    if (pushConstant.hasActiveDeform != 0) {
        SkinEntry skin = skinEntries[NonUniformResourceIndex(block)][elementIndex];
        if ((skin.flags & kDeformNoOccluder) != 0) cullFlags |= kCullNoOccluder;
        if ((skin.flags & kDeformNoCullTest) != 0) cullFlags |= kCullNoCullTest;
        if ((skin.flags & kDeformActive) != 0) {
            uint deformBase;
            InterlockedAdd(expandCounter[2], vertexSpan, deformBase);
            if (deformBase + vertexSpan > pushConstant.regionSpan) {
                skinEntries[NonUniformResourceIndex(block)][elementIndex].flags =
                    skin.flags & ~kDeformActive;
            } else {
                skinEntries[NonUniformResourceIndex(block)][elementIndex].deform_base = deformBase;
                // An even stack ends in B and an odd stack in A; expand writes
                // the final region base into the same signed bias the UV path
                // uses, so the vertex shader needs no second lookup.
                uint finalBase = deformBase +
                    (((skin.flags & kFinalRegionB) != 0) ? pushConstant.regionSpan : 0u);
                deformBias = int(finalBase) - int(baseVertex + windowBase);
            }
        }
    }
    submeshVertexEntries[NonUniformResourceIndex(block)][elementIndex].deformBias = deformBias;

    if (kCompactionMode == 0) {
        // CID: one entry per vertex slot; no per-occurrence duplication.
        for (uint j = 0; j < vertexSpan; j++) {
            vertexIndirection[entryBase + j].vertexIndex =
                (vertexBufferSlot << 24) | (baseVertex + windowBase + j);
            vertexIndirection[entryBase + j].submeshIndex = submeshIndex;
        }
    }

    // Replay the asset indices once. CID maps each occurrence to its
    // indirection slot; MID writes the absolute vertex index with no block
    // bits, which the vertex shader receives directly as SV_VertexID. The
    // 24-bit upload invariant (baseVertex + windowBase + vertexSpan < 2^24)
    // keeps every value below the maxDrawIndexedIndexValue bound.
    for (uint i = 0; i < indexRange; i++) {
        uint rawIndex = indexBuffers[NonUniformResourceIndex(indexBufSlot)][indexOffset + i];
        drawIndices[indexBase + i] = (kCompactionMode == 0)
            ? entryBase + (rawIndex - windowBase)
            : baseVertex + rawIndex;
    }

    cullEntries[NonUniformResourceIndex(block)][elementIndex].indexBase = indexBase;
    cullEntries[NonUniformResourceIndex(block)][elementIndex].indexCount = indexRange;
    cullEntries[NonUniformResourceIndex(block)][elementIndex].techniqueId = techId;
    // Bit 0 is the occluder bit, set later by occluder_select; the deform bits
    // travel with the entry so every cull pass agrees the object's rest-pose
    // bounds must not be tested.
    cullEntries[NonUniformResourceIndex(block)][elementIndex].flags = cullFlags;
}
