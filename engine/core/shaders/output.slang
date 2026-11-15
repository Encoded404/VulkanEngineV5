// Final display encode.
// Input is the engine's linear FP16 scene target, already composited with world text, screen text and ImGui.
// Output is the swapchain image, encoded exactly once for the selected (format, colorSpace) pair.
//
// The pipeline's color attachment format is the swapchain format, so the hardware applies the sRGB OETF only for _SRGB formats.
// Every other format needs the shader to apply the transfer function named by the push constant.

[[vk::binding(0, 5)]]
Sampler2D<float4> sceneColor;

struct OutputPC {
    uint encode_mode;      // OutputEncoding ordinal: 0 = hardware sRGB, 1 = shader sRGB, 2 = scRGB, 3 = HDR10 PQ.
    float exposure;
    float headroom;        // Peak divided by paper white; 1 for SDR.
    float paper_white_nits;
};

[[vk::push_constant]]
OutputPC pc;

struct VSOutput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
VSOutput vs_main(uint vertex_id : SV_VertexID) {
    // Full-screen triangle: (0,0), (2,0), (0,2).
    // The visible [0,1] region is the framebuffer, with uv.y growing down like the scene target.
    float2 p = float2(float((vertex_id << 1) & 2), float(vertex_id & 2));
    VSOutput output;
    output.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    output.uv = p;
    return output;
}

float3 SrgbEncode(float3 linear) {
    float3 low = linear * 12.92;
    float3 high = 1.055 * pow(max(linear, 0.0), 1.0 / 2.4) - 0.055;
    return lerp(high, low, step(linear, 0.0031308));
}

float3 PqEncode(float3 nits) {
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 4096.0 * 128.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 4096.0 * 32.0;
    const float c3 = 2392.0 / 4096.0 * 32.0;
    float3 y = saturate(nits / 10000.0);
    float3 ym1 = pow(y, m1);
    return pow((c1 + c2 * ym1) / (1.0 + c3 * ym1), m2);
}

// A neutral highlight knee: linear below 1.0 (paper white), smoothly compressed above it, clamped at the display's headroom.
// It maps 1 to 1 and headroom to headroom, so SDR content is unchanged and HDR highlights roll off instead of clipping.
float3 Tonemap(float3 color, float headroom) {
    color = max(color, 0.0);
    if (headroom <= 1.0) {
        return saturate(color);
    }
    const float k = 3.0;
    const float denominator = 1.0 - exp(-k);
    float3 over = max(color - 1.0, 0.0);
    float3 knee = 1.0 + (headroom - 1.0) *
                            (1.0 - exp(-k * over / (headroom - 1.0))) / denominator;
    return min(lerp(color, knee, step(1.0, color)), headroom);
}

// Linear Rec.709/sRGB primaries to linear Rec.2020.
// Written as explicit dot products rather than a float3x3 so the row/column convention of `mul` cannot silently transpose it.
// Each row sums to 1, preserving neutral white.
float3 Rec709ToRec2020(float3 c) {
    return float3(
        dot(c, float3(0.6274, 0.3293, 0.0433)),
        dot(c, float3(0.0691, 0.9195, 0.0114)),
        dot(c, float3(0.0164, 0.0880, 0.8956)));
}

[shader("fragment")]
float4 ps_main(VSOutput input) : SV_TARGET {
    float3 color = sceneColor.Sample(input.uv).rgb * max(pc.exposure, 0.0);

    if (pc.encode_mode == 0u) {
        // _SRGB image: write linear, let the hardware apply the OETF.
        return float4(saturate(color), 1.0);
    }
    if (pc.encode_mode == 1u) {
        // UNORM/FP16 image with SRGB_NONLINEAR: encode here.
        return float4(SrgbEncode(saturate(color)), 1.0);
    }

    float3 mapped = Tonemap(color, max(pc.headroom, 1.0));
    if (pc.encode_mode == 2u) {
        // scRGB: 1.0 is the 80 cd/m^2 reference white.
        // Scale paper white to that convention; the compositor maps it onto the display.
        return float4(mapped * (pc.paper_white_nits / 80.0), 1.0);
    }

    // HDR10: Rec.2020 primaries, absolute luminance, ST.2084 PQ.
    float3 rec2020 = Rec709ToRec2020(mapped);
    float3 nits = rec2020 * pc.paper_white_nits;
    return float4(PqEncode(nits), 1.0);
}
