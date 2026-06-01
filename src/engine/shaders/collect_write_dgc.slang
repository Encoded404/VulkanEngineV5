struct TechniqueResult {
    uint offset;
    uint count;
};

[[vk::binding(0, 0)]]
StructuredBuffer<TechniqueResult> intermediateResults;

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
        uint count = intermediateResults[t].count;
        if (count == 0) continue;
        uint startEntry = intermediateResults[t].offset;
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
