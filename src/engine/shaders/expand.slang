struct DynEntry {
    float3 pos;
    float pad0;
    float3 scale;
    float pad1;
    float4 rot;
};

[[vk::binding(0, 0)]]
StructuredBuffer<DynEntry> dyn[];

struct StaticEntry {
    uint indexStart;
    uint indexRange;
    uint techniqueTexture;
    uint pad;
};

[[vk::binding(1, 0)]]
StructuredBuffer<StaticEntry> st[];

struct VertEntry {
    float4x4 MVP;
    float maxScale;
    uint textureSlot;
};

[[vk::binding(2, 0)]]
RWStructuredBuffer<VertEntry> vtx[];

struct CullEntry {
    uint indirOffset;
    uint indexCount;
    uint techniqueId;
    uint pad;
};

[[vk::binding(3, 0)]]
RWStructuredBuffer<CullEntry> cull[];

struct IndirEntry {
    uint vertexId;
    uint submeshId;
};

[[vk::binding(4, 0)]]
RWStructuredBuffer<IndirEntry> indir;

[[vk::binding(5, 0)]]
globallycoherent RWStructuredBuffer<uint> dc;

struct ExpandPC {
    float4x4 vp;
    uint objectCount;
    uint pad0;
    uint pad1;
};

[[vk::push_constant]]
ConstantBuffer<ExpandPC> pc;

[[vk::binding(0, 1)]]
StructuredBuffer<uint> idx[];

float4x4 modelMat(float3 p, float4 r, float3 s) {
    float x = r.x, y = r.y, z = r.z, w = r.w;
    float xx = x * x, yy = y * y, zz = z * z;
    float xy = x * y, xz = x * z, xw = x * w;
    float yz = y * z, yw = y * w, zw = z * w;

    return float4x4(
        (1.0 - 2.0 * (yy + zz)) * s.x, 2.0 * (xy + zw) * s.x,      2.0 * (xz - yw) * s.x,      0.0,
        2.0 * (xy - zw) * s.y,          (1.0 - 2.0 * (xx + zz)) * s.y, 2.0 * (yz + xw) * s.y,      0.0,
        2.0 * (xz + yw) * s.z,           2.0 * (yz - xw) * s.z,        (1.0 - 2.0 * (xx + yy)) * s.z, 0.0,
        p.x,                             p.y,                          p.z,                         1.0
    );
}

[numthreads(64, 1, 1)]
[shader("compute")]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint id = dispatchThreadId.x;
    if (id >= pc.objectCount) return;

    uint block = id / 256u;
    uint elemIdx = id % 256u;

    float3 p = dyn[NonUniformResourceIndex(block)][elemIdx].pos;
    float3 s = dyn[NonUniformResourceIndex(block)][elemIdx].scale;
    float4 r = dyn[NonUniformResourceIndex(block)][elemIdx].rot;

    uint indexRange = st[NonUniformResourceIndex(block)][elemIdx].indexRange;
    uint techniqueTexture = st[NonUniformResourceIndex(block)][elemIdx].techniqueTexture;
    uint textureSlot = techniqueTexture >> 16;

    float maxScaleVal = max(abs(s.x), max(abs(s.y), abs(s.z)));
    float4x4 mvp = mul(pc.vp, modelMat(p, r, s));

    vtx[NonUniformResourceIndex(block)][elemIdx].MVP = mvp;
    vtx[NonUniformResourceIndex(block)][elemIdx].maxScale = maxScaleVal;
    vtx[NonUniformResourceIndex(block)][elemIdx].textureSlot = textureSlot;

    uint offsetBase;
    InterlockedAdd(dc[0], indexRange, offsetBase);

    cull[NonUniformResourceIndex(block)][elemIdx].indirOffset = offsetBase;
    cull[NonUniformResourceIndex(block)][elemIdx].indexCount = indexRange;
    cull[NonUniformResourceIndex(block)][elemIdx].techniqueId = techniqueTexture & 0xFFFFu;

    uint idxBlock = st[NonUniformResourceIndex(block)][elemIdx].indexStart >> 24;
    uint idxOffset = st[NonUniformResourceIndex(block)][elemIdx].indexStart & 0xFFFFFFu;

    for (uint i = 0; i < indexRange; i++) {
        indir[offsetBase + i].vertexId = idx[idxBlock][idxOffset + i];
        indir[offsetBase + i].submeshId = id;
    }
}
