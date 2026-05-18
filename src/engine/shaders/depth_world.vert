#version 450

// Set 0: Expanded position buffer (world-space positions from expand shader)
layout(std430, set = 0, binding = 0) readonly buffer ExpandedPositionBuffer {
    vec4 data[];
} expandedPos;

// Set 1: Camera UBO
layout(set = 1, binding = 0) uniform CameraData {
    mat4 viewProj;
    vec4 frustumPlanes[6];
} camera;

void main() {
    vec3 worldPos = expandedPos.data[gl_VertexIndex].xyz;
    gl_Position = camera.viewProj * vec4(worldPos, 1.0);
}
