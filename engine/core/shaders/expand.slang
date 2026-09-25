#include "scene_entries.slang"
#include "technique_packing.slang"

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

// [0] = next vertex-indirection base, [1] = next draw-index base. Zeroed by the CPU.
[[vk::binding(6, 0)]]
globallycoherent RWStructuredBuffer<uint, CDataLayout> expandCounter;

struct ExpandPC {
    float4x4 vp;
    uint objectCount;
    uint pad0;
    uint pad1;
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

    uint block = submeshIndex / 256u;    // which storage buffer block (max 256 submeshes per block)
    uint elementIndex = submeshIndex % 256u; // index within that block

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

    // Both modes publish the vertex buffer slot for the MID vertex shaders.
    submeshVertexEntries[NonUniformResourceIndex(block)][elementIndex].vertexBufferSlot = vertexBufferSlot;

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
    cullEntries[NonUniformResourceIndex(block)][elementIndex].flags = 0; // occluder bit, set by occluder_select
}
