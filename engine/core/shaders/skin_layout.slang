// Shared skinning layout: the GPU influence record, the per-submesh SkinEntry,
// and the flag bits that connect them to the cull passes.
//
// Every struct here must stay byte-identical to its C++ mirror in
// SceneRenderer.cppm (the static_asserts live there). This header is leaf-only
// (it includes nothing) and has no entry point: it is never compiled on its
// own, only through expand.slang / deform.slang, and is listed in
// COMPILER_DEPENDS so both rebuild when it changes.

// ── GPU influence record (12 B) ──
// Joint indices and weights are packed into 32-bit words so no
// StorageBuffer16BitAccess/8BitAccess device feature and no extra shader
// compiler flag is needed. Weights are u8, summing to 255 per set.
struct GpuBoneInfluence {
    uint joints01;  // j0 | (j1 << 16)
    uint joints23;  // j2 | (j3 << 16)
    uint weights;   // w0 | (w1 << 8) | (w2 << 16) | (w3 << 24)
};

uint InfluenceJoint0(GpuBoneInfluence b) { return b.joints01 & 0xFFFFu; }
uint InfluenceJoint1(GpuBoneInfluence b) { return b.joints01 >> 16; }
uint InfluenceJoint2(GpuBoneInfluence b) { return b.joints23 & 0xFFFFu; }
uint InfluenceJoint3(GpuBoneInfluence b) { return b.joints23 >> 16; }

float InfluenceWeight0(GpuBoneInfluence b) { return float(b.weights & 0xFFu) / 255.0; }
float InfluenceWeight1(GpuBoneInfluence b) { return float((b.weights >> 8) & 0xFFu) / 255.0; }
float InfluenceWeight2(GpuBoneInfluence b) { return float((b.weights >> 16) & 0xFFu) / 255.0; }
float InfluenceWeight3(GpuBoneInfluence b) { return float((b.weights >> 24) & 0xFFu) / 255.0; }

// ── Per-submesh deform addressing (32 B) ──
// Written by gather before the frame records; `deform_base` is filled by expand
// on the GPU, which owns the per-frame region allocator. `skin_bias` and
// `extra_bias` re-base the source vertex index into each skin table's own
// element space (table[bias + sourceIndex]), exactly like uvBias.
struct SkinEntry {
    uint palette_base;    // global matrix index into bonePalettes
    uint skin_slot;       // slot in the skin-influence table
    int  skin_bias;       // skin_region_base - source base element
    uint extra_slot;      // kNoSkinSlot when the mesh has one influence set
    int  extra_bias;
    uint influence_sets;  // 0/1/2
    uint flags;           // kDeform*/kSkinned/kFinalRegionB
    uint deform_base;     // written by expand, read by the deform stage
};

// Must equal the C++ constants in SceneRenderer.cppm.
static const uint kNoSkinSlot = 0xFFFFFFFFu;
static const uint kDeformActive = 1u << 0;       // expand allocated a region
static const uint kDeformNoOccluder = 1u << 1;
static const uint kDeformNoCullTest = 1u << 2;
static const uint kSkinned = 1u << 3;            // skin tables are valid
static const uint kFinalRegionB = 1u << 4;       // stack parity
