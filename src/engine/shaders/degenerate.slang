[shader("vertex")]
float4 vert_main() : SV_Position {
    return float4(0.0, 0.0, 2.0, 1.0);
}

[shader("fragment")]
float4 frag_main() : SV_TARGET {
    return float4(0.0, 0.0, 0.0, 0.0);
}
