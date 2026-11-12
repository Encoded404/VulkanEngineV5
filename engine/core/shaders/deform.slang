// The builtin skinning modifier (linear-blend skinning). Registered as the
// "skinning" ModifierDesc; the engine prepends it to a skinned submesh's stack
// and gathers one dispatch per stage.
//
// Addressing and layout come from modifier_common.slang. The dispatch may
// round the vertex count up to a whole workgroup, so every thread past
// pc.vertex_count returns before touching a descriptor -- otherwise the tail
// would write into the next submesh's contiguous deform region.

#include "modifier_common.slang"

[numthreads(64, 1, 1)]
[shader("compute")]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint j = dispatchThreadId.x;
    if (j >= modifierPC.vertex_count) return;

    // A submesh with no deform allocation must not write: expand clears
    // kDeformActive when the region budget could not cover it.
    SkinEntry entry = ModifierEntry(modifierPC.submesh_index);
    if ((entry.flags & (kDeformActive | kSkinned)) != (kDeformActive | kSkinned)) return;

    uint srcBase = ModifierSrc(modifierPC.stage, modifierPC.src_base,
                               entry.deform_base, modifierPC.region_span);
    uint dstBase = ModifierDst(modifierPC.stage, entry.deform_base,
                              modifierPC.region_span);

    // Two element spaces: the vertex read follows the stage (stage 0 reads the
    // undeformed table, later stages read the region the previous stage wrote),
    // while the influence records are always parallel to the *source* vertices,
    // so the skin/extra lookup uses the stage-independent source index.
    uint sourceIndex = modifierPC.src_base + j;
    uint srcIndex = srcBase + j;
    Vertex inVert;
    if (modifierPC.stage == 0u) {
        inVert = sourceVertices[NonUniformResourceIndex(modifierPC.src_slot)][sourceIndex];
    } else {
        inVert = deformVertices[srcIndex];
    }

    float3 pos = float3(inVert.px, inVert.py, inVert.pz);
    float3 n;
    float4 tPack = UnpackTBN(inVert.packedTBN, n);
    float3 tangent = tPack.xyz;
    float handedness = tPack.w;

    float3 accP = float3(0.0, 0.0, 0.0);
    float3 accN = float3(0.0, 0.0, 0.0);
    float3 accT = float3(0.0, 0.0, 0.0);
    // Vertices with no authored influence keep an all-zero record (the importer
    // stores one rather than dropping the vertex). They must pass through
    // unchanged: accumulating nothing would collapse them to the origin with a
    // NaN tangent frame.
    float totalWeight = 0.0;

    // Set 0 is always present for a skinned submesh; the extra set is read only
    // when the mesh actually has two (the 4-influence path pays one fetch).
    for (uint s = 0; s < entry.influence_sets; ++s) {
        GpuBoneInfluence bw;
        if (s == 0u) {
            bw = skinBuffers[NonUniformResourceIndex(entry.skin_slot)]
                             [entry.skin_bias + (int)sourceIndex];
        } else {
            bw = skinExtraBuffers[NonUniformResourceIndex(entry.extra_slot)]
                                 [entry.extra_bias + (int)sourceIndex];
        }

        float w0 = InfluenceWeight0(bw);
        float w1 = InfluenceWeight1(bw);
        float w2 = InfluenceWeight2(bw);
        float w3 = InfluenceWeight3(bw);
        if (w0 > 0.0) {
            totalWeight += w0;
            float4x4 m = bonePalettes[entry.palette_base + InfluenceJoint0(bw)];
            accP += w0 * mul(float4(pos, 1.0), m).xyz;
            accN += w0 * mul(n, (float3x3)m);
            accT += w0 * mul(tangent, (float3x3)m);
        }
        if (w1 > 0.0) {
            totalWeight += w1;
            float4x4 m = bonePalettes[entry.palette_base + InfluenceJoint1(bw)];
            accP += w1 * mul(float4(pos, 1.0), m).xyz;
            accN += w1 * mul(n, (float3x3)m);
            accT += w1 * mul(tangent, (float3x3)m);
        }
        if (w2 > 0.0) {
            totalWeight += w2;
            float4x4 m = bonePalettes[entry.palette_base + InfluenceJoint2(bw)];
            accP += w2 * mul(float4(pos, 1.0), m).xyz;
            accN += w2 * mul(n, (float3x3)m);
            accT += w2 * mul(tangent, (float3x3)m);
        }
        if (w3 > 0.0) {
            totalWeight += w3;
            float4x4 m = bonePalettes[entry.palette_base + InfluenceJoint3(bw)];
            accP += w3 * mul(float4(pos, 1.0), m).xyz;
            accN += w3 * mul(n, (float3x3)m);
            accT += w3 * mul(tangent, (float3x3)m);
        }
    }

    Vertex outVert;
    if (totalWeight <= 0.0) {
        // Rigid/bind-pose vertex: the source vertex is the deformed vertex.
        outVert = inVert;
        deformVertices[dstBase + j] = outVert;
        return;
    }
    outVert.px = accP.x;
    outVert.py = accP.y;
    outVert.pz = accP.z;
    outVert.u = inVert.u;
    outVert.v = inVert.v;
    // Linear blending transforms each joint's normal/tangent and renormalizes
    // the blend. That is the standard cheap approximation: the exact result
    // uses the inverse-transpose (cofactor) of the blended matrix, but the
    // difference is a small scale error that only shows under non-uniform joint
    // scale. Handedness is preserved from the input frame.
    outVert.packedTBN = PackTBN(normalize(accN), normalize(accT), handedness);

    deformVertices[dstBase + j] = outVert;
}
