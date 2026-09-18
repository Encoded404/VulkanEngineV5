// Per-instance dynamic data buffer entry: world-space transform for one submesh
struct DynEntry {
    float3 pos;
    float pad0;
    float3 scale;
    float pad1;
    float4 rot;
};

[[vk::binding(0, 0)]]
StructuredBuffer<DynEntry, CDataLayout> dynamicInstances[];

struct StaticEntry {
    uint indexStart;       // packed: hi 8 = indexBufSlot, lo 24 = indexOffset
    uint indexRange;
    uint techniqueMaterial; // packed: hi (32 - kTechniqueBits) = materialId, lo kTechniqueBits = techniqueId
    uint vertexInfo;       // packed: hi 8 = vertexBufSlot, lo 24 = baseVertex
    uint ormPacked;        // unorm8: [7:0]=AO, [15:8]=roughness, [23:16]=metallic, [31:24]=spare
    uint vertexWindowBase; // min mesh-local index referenced by the submesh (indexed-drawing window)
    uint vertexSpan;       // tight vertex-window size: max-min+1 (indexed-drawing pipeline)
};

[[vk::binding(1, 0)]]
StructuredBuffer<StaticEntry, CDataLayout> staticMeshes[];

// TechniquePacking constants — MUST match C++ TechniquePacking in BaseTechnique.cppm
static const uint kTechniqueBits = 14;
static const uint kTechniqueMask = (1u << kTechniqueBits) - 1;

// C layout (scalar block layout feature): float3x3 is 3 tightly-packed rows
// of 12 bytes (stride 12), float4x4 members are NOT 16-byte aligned —
// byte-identical to the C++ mirror struct in MeshGatherSystem.cpp:
//   MVP@0 (64B) maxScale@64 materialId@68 ormPacked@72 modelMatrix@76 (64B)
//   normalMatrix@140 (36B) -> sizeof 176.
struct VertEntry {
    float4x4 MVP;
    float maxScale;
    uint materialId;
    uint ormPacked;         // unorm8 ORM factors packed by the CPU gather pass
    float4x4 modelMatrix;   // world matrix for per-vertex world-space position computation
    float3x3 normalMatrix;  // inverse-transpose of the 3x3 of modelMatrix
};

[[vk::binding(2, 0)]]
RWStructuredBuffer<VertEntry, CDataLayout> vertexConstants[];

struct CullEntry {
    uint indexBase;   // slot base of this submesh's indices in drawIndices
    uint indexCount;
    uint techniqueId;
    uint pad;
};

[[vk::binding(3, 0)]]
RWStructuredBuffer<CullEntry, CDataLayout> cullEntries[];

struct IndirEntry {
    uint vertexId;
    uint submeshId;
};

// One entry per vertex slot in each submesh's tight window. Indexed draws make
// SV_VertexID the draw_indices value, which is a slot here.
[[vk::binding(4, 0)]]
RWStructuredBuffer<IndirEntry, CDataLayout> vertexEntries;

// Absolute slot indices into vertexEntries, one per index occurrence.
[[vk::binding(5, 0)]]
RWStructuredBuffer<uint, CDataLayout> drawIndices;

// [0] = next vertex-entry base, [1] = next draw-index base. Zeroed by the CPU.
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
    uint id = dispatchThreadId.x; // submesh index
    if (id >= pushConstant.objectCount) return;

    uint block = id / 256u;    // which storage buffer block (max 256 submeshes per block)
    uint elemIndex = id % 256u; // index within that block

    float3 position = dynamicInstances[NonUniformResourceIndex(block)][elemIndex].pos;
    float3 scale = dynamicInstances[NonUniformResourceIndex(block)][elemIndex].scale;
    float4 rotation = dynamicInstances[NonUniformResourceIndex(block)][elemIndex].rot;

    uint indexRange = staticMeshes[NonUniformResourceIndex(block)][elemIndex].indexRange;
    uint techniqueMaterial = staticMeshes[NonUniformResourceIndex(block)][elemIndex].techniqueMaterial;
    uint techId = techniqueMaterial & kTechniqueMask;
    uint matId  = techniqueMaterial >> kTechniqueBits;

    float maxScaleVal = max(abs(scale.x), max(abs(scale.y), abs(scale.z)));
    float4x4 model = modelMat(position, rotation, scale);
    float4x4 mvp = mul(model, pushConstant.vp);
    float3x3 nrm = normalMat(rotation, scale);

    vertexConstants[NonUniformResourceIndex(block)][elemIndex].MVP = mvp;
    vertexConstants[NonUniformResourceIndex(block)][elemIndex].maxScale = maxScaleVal;
    vertexConstants[NonUniformResourceIndex(block)][elemIndex].materialId = matId;
    vertexConstants[NonUniformResourceIndex(block)][elemIndex].ormPacked =
        staticMeshes[NonUniformResourceIndex(block)][elemIndex].ormPacked;
    vertexConstants[NonUniformResourceIndex(block)][elemIndex].modelMatrix = model;
    vertexConstants[NonUniformResourceIndex(block)][elemIndex].normalMatrix = nrm;

    uint indexBufSlot = staticMeshes[NonUniformResourceIndex(block)][elemIndex].indexStart >> 24;
    uint indexOffset  = staticMeshes[NonUniformResourceIndex(block)][elemIndex].indexStart & 0xFFFFFFu;
    uint vertexBufSlot = staticMeshes[NonUniformResourceIndex(block)][elemIndex].vertexInfo >> 24;
    uint baseVertex    = staticMeshes[NonUniformResourceIndex(block)][elemIndex].vertexInfo & 0xFFFFFFu;
    uint windowBase    = staticMeshes[NonUniformResourceIndex(block)][elemIndex].vertexWindowBase;
    uint vertexSpan    = staticMeshes[NonUniformResourceIndex(block)][elemIndex].vertexSpan;

    // Allocate this submesh's vertex-entry range and its draw-index range. The
    // ranges are independent: entry slots are dense over the tight vertex
    // window, index slots are dense over the index range.
    uint entryBase;
    InterlockedAdd(expandCounter[0], vertexSpan, entryBase);
    uint indexBase;
    InterlockedAdd(expandCounter[1], indexRange, indexBase);

    // One entry per vertex slot; no per-occurrence duplication.
    for (uint j = 0; j < vertexSpan; j++) {
        vertexEntries[entryBase + j].vertexId =
            (vertexBufSlot << 24) | (baseVertex + windowBase + j);
        vertexEntries[entryBase + j].submeshId = id;
    }

    // Replay the asset indices once into absolute entry slots.
    for (uint i = 0; i < indexRange; i++) {
        uint rawIndex = indexBuffers[NonUniformResourceIndex(indexBufSlot)][indexOffset + i];
        drawIndices[indexBase + i] = entryBase + (rawIndex - windowBase);
    }

    cullEntries[NonUniformResourceIndex(block)][elemIndex].indexBase = indexBase;
    cullEntries[NonUniformResourceIndex(block)][elemIndex].indexCount = indexRange;
    cullEntries[NonUniformResourceIndex(block)][elemIndex].techniqueId = techId;
    cullEntries[NonUniformResourceIndex(block)][elemIndex].pad = 0; // occluder bit, set by occluder_select
}
