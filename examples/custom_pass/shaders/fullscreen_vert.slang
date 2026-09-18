struct VSOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

// Fullscreen triangle generated from the vertex id: (-1,-1), (3,-1), (-1,3).
[shader("vertex")]
VSOutput main(uint vertex_id : SV_VertexID) {
    VSOutput output;
    float2 uv = float2(float((vertex_id << 1) & 2), float(vertex_id & 2));
    output.uv = uv;
    output.position = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    return output;
}
