#version 450

layout(location = 0) out vec4 outColor;
layout(location = 1) in vec2 inTexCoord; // Add this

void main() {
    // Simple checkered pattern based on UVs
    float check_size = 8.0;
    vec2 uv_scaled = floor(inTexCoord * check_size);
    float checker = mod(uv_scaled.x + uv_scaled.y, 2.0);
    outColor = mix(vec4(0.2, 0.2, 0.2, 1.0), vec4(0.8, 0.8, 0.8, 1.0), checker);
}
