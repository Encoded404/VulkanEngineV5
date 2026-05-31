struct CullEntry {
    uint indirOffset;
    uint indexCount;
    uint techniqueId;
    uint pad;
};

[[vk::binding(0, 0)]]
StructuredBuffer<CullEntry> cull[];

struct IndirEntry {
    uint vertexId;
    uint submeshId;
};

[[vk::binding(1, 0)]]
StructuredBuffer<IndirEntry> full;

[[vk::binding(2, 0)]]
RWStructuredBuffer<IndirEntry> comp;

[[vk::binding(3, 0)]]
RWStructuredBuffer<uint> inter;

struct CollectPC {
    uint elementCount;
    uint pad0;
    uint techniqueCount;
    uint pass;
};

[[vk::push_constant]]
ConstantBuffer<CollectPC> pc;

groupshared uint s_off[256];

void passCount(uint id) {
    if (id >= pc.elementCount) return;

    uint block = id / 256u;
    uint elemIdx = id % 256u;

    CullEntry e = cull[NonUniformResourceIndex(block)][elemIdx];
    if (e.indexCount == 0) return;

    InterlockedAdd(inter[e.techniqueId * 2u + 1u], e.indexCount);
}

void passCompact(uint id, uint localIdx) {
    if (localIdx == 0) {
        uint offset = 0;
        for (uint t = 0; t < min(pc.techniqueCount, 256u); t++) {
            uint cnt = inter[t * 2u + 1u];
            s_off[t] = offset;
            offset += cnt;
        }
    }
    AllMemoryBarrierWithGroupSync();

    if (id == 0) {
        for (uint t = 0; t < min(pc.techniqueCount, 256u); t++) {
            inter[t * 2u] = s_off[t];
            inter[t * 2u + 1u] = 0;
        }
    }
    AllMemoryBarrierWithGroupSync();

    if (id >= pc.elementCount) return;

    uint block = id / 256u;
    uint elemIdx = id % 256u;

    CullEntry e = cull[NonUniformResourceIndex(block)][elemIdx];
    if (e.indexCount == 0) return;

    uint techOffset;
    InterlockedAdd(inter[e.techniqueId * 2u + 1u], e.indexCount, techOffset);
    uint destOffset = s_off[e.techniqueId] + techOffset;

    for (uint i = 0; i < e.indexCount; i++) {
        comp[destOffset + i] = full[e.indirOffset + i];
    }
}

[numthreads(256, 1, 1)]
[shader("compute")]
void main(uint3 dispatchThreadId : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex) {
    uint id = dispatchThreadId.x;
    if (pc.pass == 0)
        passCount(id);
    else
        passCompact(id, groupIndex);
}
