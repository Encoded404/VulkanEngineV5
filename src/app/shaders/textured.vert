#version 450
#extension GL_EXT_nonuniform_qualifier : enable

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;

layout(location = 0) out vec3 outNormal;
layout(location = 1) out vec2 outTexCoord;
layout(location = 2) flat out uint outMaterialId;

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
} pc;

layout(std430, set = 1, binding = 0) readonly buffer InstanceBuffer {
    mat4 modelMatrix;
    uint materialId;
    uint _pad0;
    uint _pad1;
    uint _pad2;
} instances[];

void main() {
    mat4 model = instances[gl_InstanceIndex].modelMatrix;
    outNormal = mat3(model) * inNormal;
    outTexCoord = inTexCoord;
    outMaterialId = instances[gl_InstanceIndex].materialId;
    gl_Position = pc.viewProj * model * vec4(inPosition, 1.0);
}
