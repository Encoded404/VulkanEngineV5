// Main-pass compaction.
//
// Monolithic: two-dispatch index compaction.
//   pass 0 (count):   per-technique sum of indexCount into inter[].count.
//   pass 1 (prefix):  single thread computes the exclusive prefix into
//                     inter[].offset and resets inter[].count to 0. Doing this
//                     globally (rather than per-workgroup) is required for
//                     correctness above 256 submeshes.
//   pass 2 (compact): append each submesh's 4 B entry slots and accumulate
//                     inter[].count to get the per-technique destination.
//
// MID: single compact dispatch. Each alive submesh appends one 20 B
// DrawIndexedIndirectCommand into its technique's CPU-prefixed region and
// increments inter[].count (alive submesh count); collect_write publishes it.
struct CullEntry {
    uint indexBase;
    uint indexCount;
    uint techniqueId;
    uint pad;
};

struct TechniqueResult {
    uint offset; // monolithic: compaction base; MID: alive-submesh count scratch
    uint count;
};

[[vk::binding(0, 0)]]
StructuredBuffer<CullEntry, CDataLayout> cull[];

[[vk::binding(1, 0)]]
StructuredBuffer<uint, CDataLayout> drawIndices;

// Monolithic: packed uint index list. MID: 5 x u32 command records.
[[vk::binding(2, 0)]]
RWStructuredBuffer<uint, CDataLayout> compactedOut;

[[vk::binding(3, 0)]]
RWStructuredBuffer<TechniqueResult, CDataLayout> inter;

// MID only: CPU prefix-sum of submesh counts per technique (command regions).
[[vk::binding(4, 0)]]
StructuredBuffer<uint, CDataLayout> regionBases;

// MID only: per-technique number of commands emitted this frame. Bound in both
// modes so the descriptor layout is valid, but only written by the MID path.
[[vk::binding(5, 0)]]
RWStructuredBuffer<uint, CDataLayout> techCounts;

// 0 = monolithic, 1 = MID.
[[vk::constant_id(0)]] const uint kCompactionMode = 0;

static const uint kCmdWords = 5;

struct CollectPC {
    uint elementCount;
    uint pad0;
    uint techniqueCount;
    uint pass;
};

[[vk::push_constant]]
ConstantBuffer<CollectPC> pc;

void passCount(uint id) {
    if (id >= pc.elementCount) return;

    uint block = id / 256u;
    uint elemIdx = id % 256u;

    CullEntry entry = cull[NonUniformResourceIndex(block)][elemIdx];
    if (entry.indexCount == 0) return;

    InterlockedAdd(inter[entry.techniqueId].count, entry.indexCount);
}

// Single-thread global exclusive prefix. Also zeroes counts so the compact
// pass can use them as per-technique append cursors without a reset race.
void passPrefix() {
    uint offset = 0;
    for (uint t = 0; t < min(pc.techniqueCount, 256u); t++) {
        uint cnt = inter[t].count;
        inter[t].offset = offset;
        offset += cnt;
        inter[t].count = 0;
    }
}

void passCompactMono(uint id) {
    if (id >= pc.elementCount) return;
    uint block = id / 256u;
    uint elemIdx = id % 256u;

    CullEntry entry = cull[NonUniformResourceIndex(block)][elemIdx];
    if (entry.indexCount == 0) return;

    uint local;
    InterlockedAdd(inter[entry.techniqueId].count, entry.indexCount, local);
    uint destOffset = inter[entry.techniqueId].offset + local;
    for (uint i = 0; i < entry.indexCount; i++) {
        compactedOut[destOffset + i] = drawIndices[entry.indexBase + i];
    }
}

void passCompactMid(uint id) {
    if (id >= pc.elementCount) return;
    uint block = id / 256u;
    uint elemIdx = id % 256u;

    CullEntry entry = cull[NonUniformResourceIndex(block)][elemIdx];
    if (entry.indexCount == 0) return;

    uint slot;
    InterlockedAdd(inter[entry.techniqueId].count, 1u, slot);
    // Publish the per-technique alive-submesh count (race-free: every alive
    // submesh adds exactly once). This is the drawIndexedIndirectCount count.
    InterlockedAdd(techCounts[entry.techniqueId], 1u);
    uint cmdSlot = regionBases[entry.techniqueId] + slot;
    uint base = cmdSlot * kCmdWords;
    compactedOut[base + 0] = entry.indexCount;
    compactedOut[base + 1] = 1u;
    compactedOut[base + 2] = entry.indexBase;
    compactedOut[base + 3] = 0u;
    compactedOut[base + 4] = 0u;
}

[numthreads(256, 1, 1)]
[shader("compute")]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint id = dispatchThreadId.x;
    if (kCompactionMode == 0) {
        if (pc.pass == 0) {
            passCount(id);
        } else if (pc.pass == 1) {
            if (dispatchThreadId.x == 0) passPrefix();
        } else {
            passCompactMono(id);
        }
    } else {
        passCompactMid(id);
    }
}
