// CID main-pass epilogue: one 20 B indexed draw command per technique
// at a fixed slot (indexCount = Σ alive indices, firstIndex = compaction base).
//
// MID does not use this shader: its compact pass emits the per-submesh commands
// and publishes the per-technique alive-submesh count directly.
struct TechniqueResult {
    uint offset;
    uint count;
};

struct DrawIndexedIndirectCommand {
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int  vertexOffset;
    uint firstInstance;
};

[[vk::binding(0, 0)]]
StructuredBuffer<TechniqueResult, CDataLayout> techniqueResults;

[[vk::binding(1, 0)]]
RWStructuredBuffer<DrawIndexedIndirectCommand, CDataLayout> techniqueDrawCommands;

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
        uint count = techniqueResults[t].count;
        uint entryOffset = techniqueResults[t].offset;
        techniqueDrawCommands[t] = DrawIndexedIndirectCommand(count, 1u, entryOffset, 0, 0u);
    }
}
