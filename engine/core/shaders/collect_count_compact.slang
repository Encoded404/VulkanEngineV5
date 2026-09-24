// Main-pass compaction.
//
// Monolithic: two-dispatch index compaction.
//   pass 0 (count):   per-technique sum of indexCount into techniqueResults[].count.
//   pass 1 (prefix):  single thread computes the exclusive prefix into
//                     techniqueResults[].offset and resets techniqueResults[].count to 0.
//                     Doing this globally (rather than per-workgroup) is required for
//                     correctness above 256 submeshes.
//   pass 2 (compact): append each submesh's 4 B entry slots and accumulate
//                     techniqueResults[].count to get the per-technique destination.
//
// MID: single compact dispatch. Each alive submesh appends one 20 B
// DrawIndexedIndirectCommand into its technique's CPU-prefixed region and
// increments techniqueResults[].count (alive submesh count); collect_write
// publishes it.
#include "scene_entries.slang"

struct TechniqueResult {
    uint offset; // monolithic: compaction base; MID: alive-submesh count scratch
    uint count;
};

[[vk::binding(0, 0)]]
StructuredBuffer<CullEntry, CDataLayout> cullEntries[];

[[vk::binding(1, 0)]]
StructuredBuffer<uint, CDataLayout> drawIndices;

// Monolithic: packed uint index list. MID: 5 x u32 command records.
[[vk::binding(2, 0)]]
RWStructuredBuffer<uint, CDataLayout> mainOut;

[[vk::binding(3, 0)]]
RWStructuredBuffer<TechniqueResult, CDataLayout> techniqueResults;

// MID only: CPU prefix-sum of submesh counts per technique (command regions).
[[vk::binding(4, 0)]]
StructuredBuffer<uint, CDataLayout> techniqueRegionBases;

// MID only: per-technique number of commands emitted this frame. Bound in both
// modes so the descriptor layout is valid, but only written by the MID path.
[[vk::binding(5, 0)]]
RWStructuredBuffer<uint, CDataLayout> techniqueCounts;

#include "technique_packing.slang"

struct CollectPC {
    uint elementCount;
    uint pad0;
    uint techniqueCount;
    uint pass;
};

[[vk::push_constant]]
ConstantBuffer<CollectPC> pc;

void passCount(uint submeshIndex) {
    if (submeshIndex >= pc.elementCount) return;

    uint block = submeshIndex / 256u;
    uint elementIndex = submeshIndex % 256u;

    CullEntry entry = cullEntries[NonUniformResourceIndex(block)][elementIndex];
    if (entry.indexCount == 0) return;

    InterlockedAdd(techniqueResults[entry.techniqueId].count, entry.indexCount);
}

// Single-thread global exclusive prefix. Also zeroes counts so the compact
// pass can use them as per-technique append cursors without a reset race.
void passPrefix() {
    uint offset = 0;
    for (uint t = 0; t < min(pc.techniqueCount, 256u); t++) {
        uint cnt = techniqueResults[t].count;
        techniqueResults[t].offset = offset;
        offset += cnt;
        techniqueResults[t].count = 0;
    }
}

void passCompactMono(uint submeshIndex) {
    if (submeshIndex >= pc.elementCount) return;
    uint block = submeshIndex / 256u;
    uint elementIndex = submeshIndex % 256u;

    CullEntry entry = cullEntries[NonUniformResourceIndex(block)][elementIndex];
    if (entry.indexCount == 0) return;

    uint local;
    InterlockedAdd(techniqueResults[entry.techniqueId].count, entry.indexCount, local);
    uint destOffset = techniqueResults[entry.techniqueId].offset + local;
    for (uint i = 0; i < entry.indexCount; i++) {
        mainOut[destOffset + i] = drawIndices[entry.indexBase + i];
    }
}

void passCompactMid(uint submeshIndex) {
    if (submeshIndex >= pc.elementCount) return;
    uint block = submeshIndex / 256u;
    uint elementIndex = submeshIndex % 256u;

    CullEntry entry = cullEntries[NonUniformResourceIndex(block)][elementIndex];
    if (entry.indexCount == 0) return;

    uint slot;
    InterlockedAdd(techniqueResults[entry.techniqueId].count, 1u, slot);
    // Publish the per-technique alive-submesh count (race-free: every alive
    // submesh adds exactly once). This is the drawIndexedIndirectCount count.
    InterlockedAdd(techniqueCounts[entry.techniqueId], 1u);
    uint cmdSlot = techniqueRegionBases[entry.techniqueId] + slot;
    uint base = cmdSlot * kCmdWords;
    mainOut[base + 0] = entry.indexCount;
    mainOut[base + 1] = 1u;
    mainOut[base + 2] = entry.indexBase;
    mainOut[base + 3] = 0u;
    // firstInstance carries the submesh id; the MID vertex shader reads it from
    // the raw instance builtin (SV_StartInstanceLocation / BaseInstance).
    mainOut[base + 4] = submeshIndex;
}

[numthreads(256, 1, 1)]
[shader("compute")]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint submeshIndex = dispatchThreadId.x;
    if (kCompactionMode == 0) {
        if (pc.pass == 0) {
            passCount(submeshIndex);
        } else if (pc.pass == 1) {
            if (dispatchThreadId.x == 0) passPrefix();
        } else {
            passCompactMono(submeshIndex);
        }
    } else {
        passCompactMid(submeshIndex);
    }
}
