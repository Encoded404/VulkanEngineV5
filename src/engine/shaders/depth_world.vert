#version 450
#extension GL_EXT_nonuniform_qualifier : enable

// Per-submesh vertex data: MVP and texture (written by expand)
struct VertEntry {
    mat4 MVP;
    float maxScale;
    uint textureSlot;
};

layout(std430, set = 1, binding = 0) readonly buffer VertBuffer {
    VertEntry data[];
} sv[];

struct Vertex {
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
};

layout(std430, set = 2, binding = 0) readonly buffer VertexBuffer {
    Vertex vertices[];
} vb[];

// Indirection entry: maps a draw call index to a vertex + submesh
struct IndirEntry {
    uint vid; // (vbBlock << 24) | vertexIndex
    uint sid; // submeshId
};

layout(std430, set = 3, binding = 0) readonly buffer IndirBuffer {
    IndirEntry entries[];
} ib;

void main() {
    IndirEntry e = ib.entries[gl_VertexIndex];
    uint bufIdx = e.vid >> 24;
    uint vertIdx = e.vid & 0xFFFFFFu;
    uint submeshId = e.sid;
    uint block = submeshId / 256u;
    uint elemIdx = submeshId % 256u;
    Vertex v = vb[bufIdx].vertices[vertIdx];
    vec3 p = vec3(v.px, v.py, v.pz);

    gl_Position = sv[nonuniformEXT(block)].data[elemIdx].MVP * vec4(p, 1.0);
}
