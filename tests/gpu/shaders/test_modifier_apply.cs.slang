// GPU probe: an application vertex modifier, written to the same contract as
// engine/core/shaders/modifier_template.slang. The modifier GPU tests register
// this SPIR-V through ModifierManager and drive the real deform pass, so the
// app path (parameters, stage addressing, the A/B regions, the intra-pass
// barrier) is exercised without an app framework.
//
// The operation is selected by the parameter block rather than by separate
// entry points, which keeps one shader covering translate / scale / constant
// fill / normal displacement:
//
//   op.x < 0.5  translate by op.yzw
//   op.x < 1.5  scale about the origin by op.y
//   op.x < 2.5  write the constant position op.yzw (used to pre-fill a region
//               with a sentinel before a dispatch that must not touch it)
//   otherwise   displace along the vertex normal by op.y
//
// Include path: the GPU test shader compiler has no include search path, so the
// engine header is included by relative path; Slang resolves includes against
// the including file (test_pack_tbn.cs.slang does the same).
#include "../../../engine/core/shaders/modifier_common.slang"

// The block a ModifierDesc declares as param_size (16 B) and the CPU side
// writes into the flat parameter buffer at pc.param_offset.
struct ProbeParams {
    float4 op;
};

[numthreads(64, 1, 1)]
[shader("compute")]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint j = dispatchThreadId.x;
    // Mandatory: the dispatch rounds the vertex count up to 64 threads, and the
    // trailing threads would write into the next submesh's region.
    if (j >= modifierPC.vertex_count) return;

    const SkinEntry entry = ModifierEntry(modifierPC.submesh_index);
    // Same guard as the engine's builtin stage: a submesh whose region expand
    // could not allocate must not be written at all.
    if ((entry.flags & kDeformActive) == 0u) return;

    const uint srcBase = ModifierSrc(modifierPC.stage, modifierPC.src_base,
                                     entry.deform_base, modifierPC.region_span);
    const uint dstBase = ModifierDst(modifierPC.stage, entry.deform_base,
                                     modifierPC.region_span);

    Vertex inVert;
    if (modifierPC.stage == 0u) {
        // Unskinned submeshes still have a source table entry; the influences
        // are simply never read.
        inVert = sourceVertices[NonUniformResourceIndex(modifierPC.src_slot)]
                                [modifierPC.src_base + j];
    } else {
        inVert = deformVertices[srcBase + j];
    }

    const ProbeParams params = modifierParams.Load<ProbeParams>(modifierPC.param_offset);

    float3 pos = float3(inVert.px, inVert.py, inVert.pz);
    if (params.op.x < 0.5) {
        pos += params.op.yzw;
    } else if (params.op.x < 1.5) {
        pos *= params.op.y;
    } else if (params.op.x < 2.5) {
        pos = params.op.yzw;
    } else {
        float3 normal;
        UnpackTBN(inVert.packedTBN, normal);
        pos += normal * params.op.y;
    }

    Vertex outVert = inVert;
    outVert.px = pos.x;
    outVert.py = pos.y;
    outVert.pz = pos.z;
    // UVs and the tangent frame are carried through: a rig transform does not
    // unwrap a vertex or bend its frame.
    outVert.u = inVert.u;
    outVert.v = inVert.v;
    outVert.packedTBN = inVert.packedTBN;
    deformVertices[dstBase + j] = outVert;
}
