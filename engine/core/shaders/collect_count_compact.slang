// Main-pass compaction.
//
// CID: two-dispatch index compaction.
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
#include "scene_block_layout.slang"

struct TechniqueResult {
    uint offset; // CID: compaction base; MID: alive-submesh count scratch
    uint count;
};

[[vk::binding(0, 0)]]
StructuredBuffer<CullEntry, CDataLayout> cullEntries[];

[[vk::binding(1, 0)]]
StructuredBuffer<uint, CDataLayout> drawIndices;

// CID: packed uint index list. MID: 5 x u32 command records.
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

// A key comes from the low kTechniqueBits of a packed techniqueMaterial and is
// always below kMaxDrawGroups; the guard keeps a corrupt entry from indexing
// the technique tables out of range.
bool ValidKey(uint key) {
    return key < kMaxDrawGroups;
}

void passCount(uint submeshIndex) {
    if (submeshIndex >= pc.elementCount) return;

    uint block = submeshIndex / kEntriesPerBlock;
    uint elementIndex = submeshIndex % kEntriesPerBlock;

    CullEntry entry = cullEntries[NonUniformResourceIndex(block)][elementIndex];
    if (entry.indexCount == 0) return;
    if (!ValidKey(entry.techniqueId)) return;

    InterlockedAdd(techniqueResults[entry.techniqueId].count, entry.indexCount);
}

// Single-thread global exclusive prefix. Also zeroes counts so the compact
// pass can use them as per-technique append cursors without a reset race.
void passPrefix() {
    uint offset = 0;
    for (uint t = 0; t < min(pc.techniqueCount, kMaxDrawGroups); t++) {
        uint cnt = techniqueResults[t].count;
        techniqueResults[t].offset = offset;
        offset += cnt;
        techniqueResults[t].count = 0;
    }
}

groupshared uint g_dest;
groupshared uint g_count;

// Thread 0: reserve this submesh's range in mainOut. The copy itself is done
// by the whole workgroup in main().
void passCompactMonoReserve(uint submeshIndex) {
    if (submeshIndex >= pc.elementCount) return;
    uint block = submeshIndex / kEntriesPerBlock;
    uint elementIndex = submeshIndex % kEntriesPerBlock;

    CullEntry entry = cullEntries[NonUniformResourceIndex(block)][elementIndex];
    if (entry.indexCount == 0) return;
    if (!ValidKey(entry.techniqueId)) return;

    uint local;
    InterlockedAdd(techniqueResults[entry.techniqueId].count, entry.indexCount, local);
    g_dest = techniqueResults[entry.techniqueId].offset + local;
    g_count = entry.indexCount;
}

void passCompactMid(uint submeshIndex) {
    if (submeshIndex >= pc.elementCount) return;
    uint block = submeshIndex / kEntriesPerBlock;
    uint elementIndex = submeshIndex % kEntriesPerBlock;

    CullEntry entry = cullEntries[NonUniformResourceIndex(block)][elementIndex];
    if (entry.indexCount == 0) return;
    if (!ValidKey(entry.techniqueId)) return;

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
void main(uint3 dispatchThreadId : SV_DispatchThreadID,
          uint3 groupId : SV_GroupID,
          uint3 groupThreadId : SV_GroupThreadID) {
    // Count and prefix stay one-thread-per-submesh / single-thread: O(1) work.
    if (kCompactionMode == 0 && pc.pass == 0) {
        passCount(dispatchThreadId.x);
        return;
    }
    if (kCompactionMode == 0 && pc.pass == 1) {
        if (dispatchThreadId.x == 0) passPrefix();
        return;
    }

    // Compact: one workgroup per submesh so the index copy is parallel.
    const uint submeshIndex = groupId.x;
    const uint tid = groupThreadId.x;

    if (tid == 0) {
        g_dest = 0;
        g_count = 0;
        if (kCompactionMode == 0) {
            passCompactMonoReserve(submeshIndex);
        } else {
            passCompactMid(submeshIndex);
        }
    }
    GroupMemoryBarrierWithGroupSync();

    if (kCompactionMode == 0 && g_count != 0) {
        uint block = submeshIndex / kEntriesPerBlock;
        uint elementIndex = submeshIndex % kEntriesPerBlock;
        CullEntry entry = cullEntries[NonUniformResourceIndex(block)][elementIndex];
        for (uint i = tid; i < g_count; i += 256) {
            mainOut[g_dest + i] = drawIndices[entry.indexBase + i];
        }
    }
}
