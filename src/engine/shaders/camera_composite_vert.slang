struct VSOutput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

// Fullscreen triangle generated from gl_VertexIndex — no vertex buffer.
// The dynamic viewport (set per binding) maps the triangle to the
// destination rectangle; `uv` spans [0,1] across that viewport.
[shader("vertex")]
VSOutput main(uint vertexID : SV_VertexID) {
    VSOutput o;
    float2 uv = float2((vertexID << 1) & 2, vertexID & 2);
    o.position = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    o.uv = uv;
    return o;
}
