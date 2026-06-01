// Two-pass compaction: count visible entries per technique, then scatter them into a packed buffer.
struct CullEntry {
    uint indirOffset;
    uint indexCount;
    uint techniqueId;
    uint pad;
};

struct TechniqueResult {
    uint offset;
    uint count;
};

[[vk::binding(0, 0)]]
StructuredBuffer<CullEntry> cull[];

struct IndirEntry {
    uint vertexId;
    uint submeshId;
};

[[vk::binding(1, 0)]]
StructuredBuffer<IndirEntry> fullIndirect;

[[vk::binding(2, 0)]]
RWStructuredBuffer<IndirEntry> compactedIndirect;

[[vk::binding(3, 0)]]
RWStructuredBuffer<TechniqueResult> inter;

struct CollectPC {
    uint elementCount;
    uint pad0;
    uint techniqueCount;
    uint pass;
};

[[vk::push_constant]]
ConstantBuffer<CollectPC> pc;

groupshared uint sharedOffsets[256];

void passCount(uint id) {
    if (id >= pc.elementCount) return;

    uint block = id / 256u;
    uint elemIdx = id % 256u;

    CullEntry entry = cull[NonUniformResourceIndex(block)][elemIdx];
    if (entry.indexCount == 0) return;

    InterlockedAdd(inter[entry.techniqueId].count, entry.indexCount);
}

void passCompact(uint id, uint localIdx) {
    if (localIdx == 0) {
        uint offset = 0;
        for (uint t = 0; t < min(pc.techniqueCount, 256u); t++) {
            uint cnt = inter[t].count;
            sharedOffsets[t] = offset;
            offset += cnt;
        }
    }
    AllMemoryBarrierWithGroupSync();

    if (id == 0) {
        for (uint t = 0; t < min(pc.techniqueCount, 256u); t++) {
            inter[t].offset = sharedOffsets[t];
            inter[t].count = 0;
        }
    }
    AllMemoryBarrierWithGroupSync();

    if (id >= pc.elementCount) return;

    uint block = id / 256u;
    uint elemIdx = id % 256u;

    CullEntry entry = cull[NonUniformResourceIndex(block)][elemIdx];
    if (entry.indexCount == 0) return;

    uint techniqueOffset;
    InterlockedAdd(inter[entry.techniqueId].count, entry.indexCount, techniqueOffset);
    uint destOffset = sharedOffsets[entry.techniqueId] + techniqueOffset;

    for (uint i = 0; i < entry.indexCount; i++) {
        compactedIndirect[destOffset + i] = fullIndirect[entry.indirOffset + i];
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
