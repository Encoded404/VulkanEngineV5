// Shared contract for the builtin skinning modifier and every application
// modifier. It declares the two descriptor sets the deform pipeline layout
// always has, the 32-byte ModifierPC push constant, and the stage addressing
// helpers. An app modifier includes this file and nothing else of the deform
// path.
//
// Stage addressing (all element indices, never byte offsets):
//   A = entry.deform_base           (allocated by expand)
//   B = entry.deform_base + pc.region_span
//   dst(stage) = (stage & 1) ? B : A
//   src(stage) = stage == 0 ? pc.src_base : ((stage & 1) ? A : B)
// Two regions are enough for any number of 1:1 stages because every stage reads
// its source region and writes its destination at the same element index. A
// stage must never read the destination region it is writing (see
// docs/skeletal-animation.md); sampling the source region at other indices is
// allowed.
//
// kEntriesPerBlock comes from scene_block_layout.slang, SkinEntry from
// skin_layout.slang, Vertex from scene_entries.slang.

#include "scene_entries.slang"
#include "scene_block_layout.slang"
#include "skin_layout.slang"
#include "normal_encoding.slang"
#include "normal_pack.slang"

// set 0 -- the existing vertex-buffer table layout (both bindings of that
// layout are visible to compute; the UV binding is simply unused here).
[[vk::binding(0, 0)]]
StructuredBuffer<Vertex, CDataLayout> sourceVertices[];

// set 1 -- deform inputs and outputs.
[[vk::binding(0, 1)]]
StructuredBuffer<GpuBoneInfluence, CDataLayout> skinBuffers[];

[[vk::binding(1, 1)]]
StructuredBuffer<GpuBoneInfluence, CDataLayout> skinExtraBuffers[];

// Flat palette: one mat4 per joint, addressed by one global index
// (entry.palette_base + joint). Not a block array: a palette may start in the
// middle of a block or cross a block boundary.
[[vk::binding(2, 1)]]
StructuredBuffer<float4x4, CDataLayout> bonePalettes;

[[vk::binding(3, 1)]]
StructuredBuffer<SkinEntry, CDataLayout> skinEntries[];

[[vk::binding(4, 1)]]
RWStructuredBuffer<Vertex, CDataLayout> deformVertices;

// Read-only per-instance parameter block (ByteAddressBuffer, so
// modifierParams.Load<T>(pc.param_offset + k) is 4-byte aligned by contract).
[[vk::binding(5, 1)]]
ByteAddressBuffer modifierParams;

// Push constants of every modifier dispatch (32 B, all 32-bit words).
struct ModifierPC {
    uint submesh_index;
    uint stage;
    uint src_slot;      // vertex-buffer table slot stage 0 reads
    uint src_base;      // absolute source element index stage 0 reads
    uint vertex_count;  // submesh tight-window span; dispatch rounds up
    uint param_offset;  // byte offset into modifierParams
    uint param_size;
    uint region_span;   // T: elements per deform region, not 2T
};

[[vk::push_constant]]
ConstantBuffer<ModifierPC> modifierPC;

// Even stages write A (deform_base), odd stages B (deform_base + region_span).
uint ModifierDst(uint stage, uint deformBase, uint regionSpan) {
    return (stage & 1u) != 0u ? deformBase + regionSpan : deformBase;
}

// Stage 0 reads the source vertex table; later stages read the region the
// previous stage wrote: odd stages read A, even stages read B.
uint ModifierSrc(uint stage, uint srcBase, uint deformBase, uint regionSpan) {
    if (stage == 0u) return srcBase;
    return (stage & 1u) != 0u ? deformBase : deformBase + regionSpan;
}

// The SkinEntry of the dispatch's submesh.
SkinEntry ModifierEntry(uint submeshIndex) {
    uint block = submeshIndex / kEntriesPerBlock;
    uint elementIndex = submeshIndex % kEntriesPerBlock;
    return skinEntries[NonUniformResourceIndex(block)][elementIndex];
}
