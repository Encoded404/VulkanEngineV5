struct TechniqueResult {
    uint offset;
    uint count;
};

struct DrawIndirectCommand {
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int vertexOffset;
};

[[vk::binding(0, 0)]]
StructuredBuffer<TechniqueResult> intermediateResults;

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
        uint count = intermediateResults[t].count;
        uint entryOffset = intermediateResults[t].offset;
        tech_counts[t] = count;
        tech_offsets[t] = entryOffset;
        tech_cmds[t] = DrawIndirectCommand(count, 1u, entryOffset, 0);
    }
}
