// Shared scene-entry struct definitions, used verbatim by the expand/cull/
// compaction passes and the indirection vertex shaders. Every struct here must
// stay byte-identical to its C++ mirror in MeshGatherSystem.cpp (size
// static_asserts live there); when editing, keep all mirrors in sync.
//
// This header is leaf-only (it includes nothing) and has no entry point: it is
// never compiled on its own and is not part of the _engine_shaders list, only
// of COMPILER_DEPENDS so every dependent shader rebuilds when it changes.

// Per-instance dynamic data buffer entry: world-space transform for one submesh
struct DynamicEntry {
    float3 pos;
    float pad0;
    float3 scale;
    float pad1;
    float4 rot;
};

struct StaticEntry {
    uint indexStartPacked; // packed: hi 8 = indexBufSlot, lo 24 = indexOffset
    uint indexRange;
    uint techniqueMaterial; // packed: hi (32 - kTechniqueBits) = materialId, lo kTechniqueBits = techniqueId
    uint vertexInfoPacked; // packed: hi 8 = vertexBufferSlot, lo 24 = baseVertex
    uint ormPacked;        // unorm8: [7:0]=AO, [15:8]=roughness, [23:16]=metallic, [31:24]=spare
    uint vertexWindowBase; // min mesh-local index referenced by the submesh (indexed-drawing window)
    uint vertexSpan;       // tight vertex-window size: max-min+1 (indexed-drawing pipeline)
};

// C layout (scalar block layout feature): float3x3 is 3 tightly-packed rows
// of 12 bytes (stride 12), float4x4 members are NOT 16-byte aligned —
// byte-identical to the C++ mirror struct in MeshGatherSystem.cpp:
//   mvp@0 (64B) maxScale@64 materialId@68 ormPacked@72 modelMatrix@76 (64B)
//   normalMatrix@140 (36B) -> sizeof 176.
struct VertexEntry {
    float4x4 mvp;
    float maxScale;
    uint materialId;
    uint ormPacked;         // unorm8 ORM factors packed by the CPU gather pass
    float4x4 modelMatrix;   // world matrix for per-vertex world-space position computation
    float3x3 normalMatrix;  // inverse-transpose of the 3x3 of modelMatrix
};

struct CullEntry {
    uint indexBase;   // slot base of this submesh's indices in drawIndices
    uint indexCount;
    uint techniqueId;
    uint flags;       // occluder bit, set by occluder_select
};

// One field spelling for all three users: the expand pass writes the packed
// buffer slot and mesh-local vertex index, the indirection vertex shaders
// decode them.
struct VertexIndirectionEntry {
    uint vertexIndex; // packed: hi 8 = vertexBufferSlot, lo 24 = mesh-local vertex index
    uint submeshIndex;
};

// Oriented bounding box: center + 3 half-extent axes (xyz = axis, w = half-length),
// model space. C layout matches the CPU ObbEntry mirror (4 x float4, 64 B).
struct ObbEntry {
    float4 center_pad;
    float4 u_half;
    float4 v_half;
    float4 w_half;
};

struct Vertex {
    float px, py, pz;
    uint packedTBN;         // packed tangent frame, see normal_encoding.slang
    float u, v;
};
