#include "scene_entries.slang"
#include "scene_block_layout.slang"
#include "technique_packing.slang"   // kCompactionMode (constant_id 0): draw-mode selector

[[vk::binding(0, 1)]]
StructuredBuffer<VertexEntry, CDataLayout> submeshVertexEntries[];

[[vk::binding(0, 2)]]
StructuredBuffer<Vertex, CDataLayout> vertexBuffers[];

// Per-frame deformed-vertex output (engine set 2, binding 2). See
// main_indir.slang: a non-sentinel deformBias in the entry redirects the read.
[[vk::binding(2, 2)]]
StructuredBuffer<Vertex, CDataLayout> deformVertices;

[[vk::binding(0, 3)]]
StructuredBuffer<VertexIndirectionEntry, CDataLayout> vertexIndirection;

[shader("vertex")]
float4 main(uint vertexId : SV_VertexID,
            uint submeshIndex : SV_StartInstanceLocation) : SV_Position {
    // See main_indir.slang: CID reads the indirection entry, MID takes
    // the vertex index from SV_VertexID and the submesh id from the raw
    // instance builtin (never the zero-based SV_InstanceID).
    uint vertexIndex;
    uint indirectionSlot = 0;
    uint block;
    uint elementIndex;
    if (kCompactionMode == 0) {
        VertexIndirectionEntry entry = vertexIndirection[vertexId];
        indirectionSlot = entry.vertexIndex >> 24;
        vertexIndex = entry.vertexIndex & 0xFFFFFFu;
        submeshIndex = entry.submeshIndex;
        block = submeshIndex / kEntriesPerBlock;
        elementIndex = submeshIndex % kEntriesPerBlock;
    } else {
        vertexIndex = vertexId;
        block = submeshIndex / kEntriesPerBlock;
        elementIndex = submeshIndex % kEntriesPerBlock;
    }
    // Hoisted above the vertex fetch so the deform bias is known first.
    VertexEntry info = submeshVertexEntries[NonUniformResourceIndex(block)][elementIndex];
    uint vertexBufferSlot = (kCompactionMode == 0) ? indirectionSlot : info.vertexBufferSlot;
    Vertex vert = (info.deformBias != kNoDeform)
        ? deformVertices[info.deformBias + (int)vertexIndex]
        : vertexBuffers[NonUniformResourceIndex(vertexBufferSlot)][vertexIndex];
    float3 p = float3(vert.px, vert.py, vert.pz);
    return mul(float4(p, 1.0), info.mvp);
}
