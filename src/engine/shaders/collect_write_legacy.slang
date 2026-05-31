struct DrawIndirectCommand {
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int vertexOffset;
};

[[vk::binding(0, 0)]]
StructuredBuffer<uint> inter;

[[vk::binding(1, 0)]]
RWStructuredBuffer<uint> tech_counts;

[[vk::binding(2, 0)]]
RWStructuredBuffer<uint> tech_offsets;

[[vk::binding(3, 0)]]
RWStructuredBuffer<DrawIndirectCommand> tech_cmds;

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
    for (uint t = 0; t < pc.techniqueCount; t++) {
        uint cnt = inter[t * 2u + 1u];
        uint startEntry = inter[t * 2u];
        tech_counts[t] = cnt;
        tech_offsets[t] = startEntry;
        tech_cmds[t] = DrawIndirectCommand(cnt, 1u, startEntry, 0);
    }
}
