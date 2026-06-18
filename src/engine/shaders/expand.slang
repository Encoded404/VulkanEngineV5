// Per-instance dynamic data buffer entry: world-space transform for one submesh
struct DynEntry {
    float3 pos;
    float pad0;
    float3 scale;
    float pad1;
    float4 rot;
};

[[vk::binding(0, 0)]]
StructuredBuffer<DynEntry> dynamicInstances[];

struct StaticEntry {
    uint indexStart;       // packed: hi 8 = indexBufSlot, lo 24 = indexOffset
    uint indexRange;
    uint techniqueMaterial; // packed: hi 16 = materialId, lo 16 = techniqueId
    uint vertexInfo;       // packed: hi 8 = vertexBufSlot, lo 24 = baseVertex
};

[[vk::binding(1, 0)]]
StructuredBuffer<StaticEntry> staticMeshes[];

struct VertEntry {
    float4x4 MVP;
    float maxScale;
    uint materialId;
};

[[vk::binding(2, 0)]]
RWStructuredBuffer<VertEntry> vertexConstants[];

struct CullEntry {
    uint indirOffset;
    uint indexCount;
    uint techniqueId;
    uint pad;
};

[[vk::binding(3, 0)]]
RWStructuredBuffer<CullEntry> cullEntries[];

struct IndirEntry {
    uint vertexId;
    uint submeshId;
};

[[vk::binding(4, 0)]]
RWStructuredBuffer<IndirEntry> indirectDraws;

[[vk::binding(5, 0)]]
globallycoherent RWStructuredBuffer<uint> drawCounter;

struct ExpandPC {
    float4x4 vp;
    uint objectCount;
    uint pad0;
    uint pad1;
};

[[vk::push_constant]]
ConstantBuffer<ExpandPC> pushConstant;

[[vk::binding(0, 1)]]
StructuredBuffer<uint> indexBuffers[];

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
    uint techId = techniqueMaterial & 0xFFFFu;
    uint matId  = techniqueMaterial >> 16;

    float maxScaleVal = max(abs(scale.x), max(abs(scale.y), abs(scale.z)));
    float4x4 mvp = mul(modelMat(position, rotation, scale), pushConstant.vp);

    vertexConstants[NonUniformResourceIndex(block)][elemIndex].MVP = mvp;
    vertexConstants[NonUniformResourceIndex(block)][elemIndex].maxScale = maxScaleVal;
    vertexConstants[NonUniformResourceIndex(block)][elemIndex].materialId = matId;

    uint drawOffset;
    InterlockedAdd(drawCounter[0], indexRange, drawOffset); // allocate range in the indirect draw buffer

    cullEntries[NonUniformResourceIndex(block)][elemIndex].indirOffset = drawOffset;
    cullEntries[NonUniformResourceIndex(block)][elemIndex].indexCount = indexRange;
    cullEntries[NonUniformResourceIndex(block)][elemIndex].techniqueId = techId;

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
