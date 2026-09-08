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
    uint techniqueMaterial; // packed: hi 16 = materialId, lo 16 = techniqueId
    uint vertexInfo;       // packed: hi 8 = vertexBufSlot, lo 24 = baseVertex
    uint ormPacked;        // unorm8: [7:0]=AO, [15:8]=roughness, [23:16]=metallic, [31:24]=spare
};

[[vk::binding(1, 0)]]
StructuredBuffer<StaticEntry, CDataLayout> staticMeshes[];

// TechniquePacking constants — MUST match C++ TechniquePacking in TechniqueManager.cppm
static const uint kTechniqueBits = 12;
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
    uint indirOffset;
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

[[vk::binding(4, 0)]]
RWStructuredBuffer<IndirEntry, CDataLayout> indirectDraws;

[[vk::binding(5, 0)]]
globallycoherent RWStructuredBuffer<uint, CDataLayout> drawCounter;

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

    uint drawOffset;
    InterlockedAdd(drawCounter[0], indexRange, drawOffset); // allocate range in the indirect draw buffer

    cullEntries[NonUniformResourceIndex(block)][elemIndex].indirOffset = drawOffset;
    cullEntries[NonUniformResourceIndex(block)][elemIndex].indexCount = indexRange;
    cullEntries[NonUniformResourceIndex(block)][elemIndex].techniqueId = techId;
    cullEntries[NonUniformResourceIndex(block)][elemIndex].pad = 0; // occluder bit, set by occluder_select

    uint indexBufSlot = staticMeshes[NonUniformResourceIndex(block)][elemIndex].indexStart >> 24;
    uint indexOffset  = staticMeshes[NonUniformResourceIndex(block)][elemIndex].indexStart & 0xFFFFFFu;
    uint vertexBufSlot = staticMeshes[NonUniformResourceIndex(block)][elemIndex].vertexInfo >> 24;
    uint baseVertex    = staticMeshes[NonUniformResourceIndex(block)][elemIndex].vertexInfo & 0xFFFFFFu;

    for (uint i = 0; i < indexRange; i++) {
        uint rawIndex = indexBuffers[indexBufSlot][indexOffset + i];
        indirectDraws[drawOffset + i].vertexId = (vertexBufSlot << 24) | (rawIndex + baseVertex);
        indirectDraws[drawOffset + i].submeshId = id;
    }
}
