#version 450
#extension GL_EXT_nonuniform_qualifier : enable

// Reads world-space positions and packed attributes from expanded SSBOs (set 2)
// Applies only viewProj — positions are already in world space.

layout(location = 0) out vec3 outNormal;
layout(location = 1) out vec2 outTexCoord;
layout(location = 2) flat out uint outMaterialId;

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
} pc;

// Set 0: Bindless textures (bound globally, same as depth pass)
// Set 1: Instance data (techniqueId only — modelMatrix is identity for world-space path)
layout(std430, set = 1, binding = 0) readonly buffer InstanceBuffer {
    mat4 modelMatrix;
    uint techniqueId;
    uint _pad0;
    uint _pad1;
    uint _pad2;
} instances[];

// Set 2: Expanded buffers
layout(std430, set = 2, binding = 0) readonly buffer ExpandedPositionBuffer {
    vec4 data[];
} expandedPos;

layout(std430, set = 2, binding = 1) readonly buffer ExpandedAttributeBuffer {
    uvec4 data[];
} expandedAttrib;

void main() {
    vec4 posData = expandedPos.data[gl_VertexIndex];
    vec3 worldPos = posData.xyz;
    outMaterialId = floatBitsToUint(posData.w);

    uvec4 attrib = expandedAttrib.data[gl_VertexIndex];

    // Unpack octahedral normal
    uint nx = attrib.x & 0xFFu;
    uint ny = (attrib.x >> 8) & 0xFFu;
    vec3 normal;
    normal.xy = vec2(nx, ny) / 127.0 - 1.0;
    float nz = 1.0 - abs(normal.x) - abs(normal.y);
    if (nz >= 0.0) {
        normal.z = nz;
    } else {
        normal.xy = (1.0 - abs(normal.yx)) * sign(normal.xy);
        normal.z = nz;
    }
    outNormal = normalize(normal);

    // Unpack half-float texcoord
    outTexCoord = unpackHalf2x16(attrib.y);

    gl_Position = pc.viewProj * vec4(worldPos, 1.0);
}
