#version 450
#extension GL_EXT_nonuniform_qualifier : enable

// Indirection entry: maps a draw call index to a vertex + submesh
struct IndirEntry {
    uint vid; // (vbBlock << 24) | vertexIndex
    uint sid; // submeshId
};

layout(std430, set = 3, binding = 0) readonly buffer IndirBuffer {
    IndirEntry entries[];
} ib;

// Per-submesh vertex data: MVP and texture (written by expand)
struct VertEntry {
    mat4 MVP;
    float maxScale;
    uint textureSlot;
};

layout(std430, set = 1, binding = 0) readonly buffer VertBuffer {
    VertEntry data[];
} sv[];

// Raw vertex buffers: interleaved pos3 + nrm3 + tex2 as raw uints
layout(std430, set = 2, binding = 0) readonly buffer VertexBuffer {
    uint d[];
} vb[];

void main() {
    IndirEntry e = ib.entries[gl_VertexIndex];
    uint bufIdx = e.vid >> 24;
    uint vertIdx = e.vid & 0xFFFFFFu;
    uint submeshId = e.sid;
    uint block = submeshId / 256u;
    uint elemIdx = submeshId % 256u;
    uint vertBase = vertIdx * 8u;

    vec3 p = vec3(
        uintBitsToFloat(vb[bufIdx].d[vertBase]),
        uintBitsToFloat(vb[bufIdx].d[vertBase + 1u]),
        uintBitsToFloat(vb[bufIdx].d[vertBase + 2u])
    );

    gl_Position = sv[nonuniformEXT(block)].data[elemIdx].MVP * vec4(p, 1.0);
}
