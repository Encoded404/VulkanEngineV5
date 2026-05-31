[[vk::binding(0, 0)]]
StructuredBuffer<uint> inter;

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
    uint packedIdx = 0;
    for (uint t = 0; t < pc.techniqueCount; t++) {
        uint cnt = inter[t * 2u + 1u];
        if (cnt == 0) continue;
        uint startEntry = inter[t * 2u];
        uint seq_off = packedIdx * 5u;
        dgc_seq[seq_off + 0u] = t;
        dgc_seq[seq_off + 1u] = cnt;
        dgc_seq[seq_off + 2u] = 1u;
        dgc_seq[seq_off + 3u] = startEntry;
        dgc_seq[seq_off + 4u] = 0u;
        packedIdx++;
    }
    dgc_cnt[0] = packedIdx;
}
