#version 450

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec2 inTexCoord;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D diffuseTexture;

void main() {
    vec4 texColor = texture(diffuseTexture, inTexCoord);
    if (texColor.a < 0.1) {
        discard;
    }
    outColor = texColor;
}
