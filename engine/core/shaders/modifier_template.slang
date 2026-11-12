// Application vertex-modifier example, and the contract every app modifier
// follows. Copy this file, change `MyParams` and the body, and register the
// result:
//
//   ShaderSystem::ShaderId id = shaders.RegisterManual(
//       "my_modifier.spv", "my_modifier.slang", ShaderStage::eCompute);
//   engine.RegisterModifier(ModifierDesc{
//       .name = "my_modifier",
//       .compute_shader = id,
//       .entry_point = "main",
//       .param_size = sizeof(MyParams),      // must match this shader's block
//       .param_alignment = 16,
//       .workgroup_size = 64,                // must match [numthreads]
//   });
//
// A GeometryModifier stack then names the returned ModifierId and supplies one
// ModifierInstance::params block per invocation.
//
// Contract:
//
//  * `[numthreads(N, 1, 1)]` must equal ModifierDesc::workgroup_size. The engine
//    dispatches ceil(vertex_count / N) whole workgroups.
//  * The stack is 1:1 in vertices: a modifier may move, re-pack or retint a
//    vertex but never add, remove or reorder one.
//  * Declare only the bindings you use. The engine's deform pipeline layout
//    always has all six of the ones modifier_common.slang declares, so a shader
//    that reads just `modifierParams` and `deformVertices` still links.
//  * Stage 0 reads the submesh's source vertices; stage k > 0 reads the region
//    stage k-1 wrote. ModifierSrc/ModifierDst are the only correct way to
//    compute those bases: they own the A/B region parity.
//  * Never read the destination region a dispatch is writing. That read races
//    the in-flight dispatch and is driver-dependent. Sampling the *source*
//    region at neighbouring indices (a blur, a smoothing pass) is allowed: that
//    region is complete when the dispatch starts.
//  * Return before writing unless the submesh's SkinEntry still has
//    kDeformActive. expand clears that bit when the frame's region budget cannot
//    cover the submesh; the dispatches still run, and a write would land on the
//    region base gather left in the entry (0), overwriting another submesh's
//    deformed vertices.
//  * Preserve UVs, and re-pack `packedTBN` with PackTBN when the frame changes.
//  * Read parameters with `modifierParams.Load<T>(pc.param_offset + k)`; the
//    block is 4-byte aligned and read-only.
//
// The early-out below is not optional.

#include "modifier_common.slang"

// Bytes laid out exactly like ModifierInstance::params: the engine copies the
// instance's block into the frame's flat parameter buffer at pc.param_offset.
struct MyParams {
    float4 wave; // x = amplitude, y = frequency, z = phase, w = unused padding
};

[numthreads(64, 1, 1)]
[shader("compute")]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint j = dispatchThreadId.x;
    // MANDATORY. The dispatch rounds the vertex count up to a whole workgroup;
    // without this guard the trailing threads write into the next submesh's
    // contiguous deform region.
    if (j >= modifierPC.vertex_count) return;

    const SkinEntry entry = ModifierEntry(modifierPC.submesh_index);
    // expand owns the region allocator, and it clears kDeformActive when the
    // frame cannot cover this submesh. The dispatch still runs: without this
    // check the write below would use the stale base and corrupt another
    // submesh's region instead of letting the object render its bind pose.
    if ((entry.flags & kDeformActive) == 0u) return;

    const uint srcBase = ModifierSrc(modifierPC.stage, modifierPC.src_base,
                                     entry.deform_base, modifierPC.region_span);
    const uint dstBase = ModifierDst(modifierPC.stage, entry.deform_base,
                                     modifierPC.region_span);

    Vertex inVert;
    if (modifierPC.stage == 0u) {
        // The influence records are parallel to the source vertices, so this is
        // the stage-independent source element index.
        inVert = sourceVertices[NonUniformResourceIndex(modifierPC.src_slot)]
                                [modifierPC.src_base + j];
    } else {
        // Source region, not the destination region: the previous stage's write
        // is complete here.
        inVert = deformVertices[srcBase + j];
    }

    const MyParams p = modifierParams.Load<MyParams>(modifierPC.param_offset);

    float3 pos = float3(inVert.px, inVert.py, inVert.pz);
    float3 normal;
    const float4 frame = UnpackTBN(inVert.packedTBN, normal);

    // A wave along the vertex normal. Translation leaves the tangent frame
    // intact, so packedTBN is carried through unchanged below; an operation that
    // rotates or scales the vertex has to transform `frame.xyz` too and re-pack
    // it with PackTBN(newNormal, newTangent, frame.w).
    pos += normal * (p.wave.x * sin(p.wave.y * pos.y + p.wave.z));

    Vertex outVert = inVert;
    outVert.px = pos.x;
    outVert.py = pos.y;
    outVert.pz = pos.z;
    // UVs are not a deform output: the 1:1 stack carries them through.
    outVert.u = inVert.u;
    outVert.v = inVert.v;
    outVert.packedTBN = inVert.packedTBN;

    // Write only this dispatch's own element range.
    deformVertices[dstBase + j] = outVert;
}
