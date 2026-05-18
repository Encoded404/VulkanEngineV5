#version 450
#extension GL_EXT_nonuniform_qualifier : enable

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec2 inTexCoord;
layout(location = 2) flat in uint inMaterialId;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D allTextures[256];

void main() {
    vec4 texColor = texture(allTextures[nonuniformEXT(inMaterialId)], inTexCoord);
    outColor = texColor;
}
