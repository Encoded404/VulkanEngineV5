#version 450
#extension GL_EXT_nonuniform_qualifier : enable

layout(location = 0) out vec3 outN;
layout(location = 1) out vec2 outT;
layout(location = 2) flat out uint outM;

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
    vec3 n = vec3(v.nx, v.ny, v.nz);
    vec2 t = vec2(v.u, v.v);

    VertEntry s = sv[nonuniformEXT(block)].data[elemIdx];
    outM = s.textureSlot;
    outN = n;
    outT = t;
    gl_Position = s.MVP * vec4(p, 1.0);
}
