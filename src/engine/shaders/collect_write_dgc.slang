// Packs per-technique counts/offsets into DGC (Device-Generated Command) sequences.
[[vk::binding(0, 0)]]
StructuredBuffer<uint> intermediateResults;

[[vk::binding(1, 0)]]
RWStructuredBuffer<uint> dgc_seq;

[[vk::binding(2, 0)]]
RWStructuredBuffer<uint> dgc_cnt;

struct WritePC {
    uint elementCount;
    uint pad0;
    uint techniqueCount;
    uint pad1;
};

[[vk::push_constant]]
ConstantBuffer<WritePC> pc;

[numthreads(1, 1, 1)]
[shader("compute")]
void main() {
    uint packedCommandIdx = 0;
    for (uint t = 0; t < pc.techniqueCount; t++) {
        uint count = intermediateResults[t * 2u + 1u];
        if (count == 0) continue;
        uint startEntry = intermediateResults[t * 2u];
        uint seqOffset = packedCommandIdx * 5u;
        dgc_seq[seqOffset + 0u] = t;
        dgc_seq[seqOffset + 1u] = count;
        dgc_seq[seqOffset + 2u] = 1u;
        dgc_seq[seqOffset + 3u] = startEntry;
        dgc_seq[seqOffset + 4u] = 0u;
        packedCommandIdx++;
    }
    dgc_cnt[0] = packedCommandIdx;
}
