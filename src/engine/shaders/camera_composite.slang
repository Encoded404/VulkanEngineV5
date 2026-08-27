// PhysicalCamera composite fragment shader.
//
// Draws a camera frame (kept in a native format on the GPU — no CPU
// conversion) into a color-attachment target texture, converting to RGB
// on the GPU. The source is sampled from the engine's bindless texture
// array (set 0, binding 0); the target is the pass's color attachment.
//
// Supported source formats (see PhysicalCameraPixelFormat):
//   0 = Xrgb8888   single B8G8R8A8 image, trivial swizzle
//   1 = Yuy2       packed 4:2:2, stored as R8G8B8A8 half-width image
//                  (R=Y0 G=U B=Y1 A=V), YUV->RGB in-shader
//   2 = Nv12       two images: R8 Y plane (slot0) + RG8 interleaved
//                  UV plane (slot1), 4:2:0, YUV->RGB in-shader

[[vk::binding(0, 0)]]
Texture2D<float4> allTextures[];
[[vk::binding(0, 0)]]
SamplerState cameraSampler;

struct VSOutput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

struct CameraCompositePC {
    uint format;       // 0 = Xrgb8888, 1 = Yuy2, 2 = Nv12
    uint src_slot0;    // bindless slot of the primary image
    uint src_slot1;    // bindless slot of the secondary image (Nv12 UV) — unused otherwise
    uint flags;        // bit0 = flip_x, bit1 = flip_y
    float4 src_rect;   // source crop in source pixels: (x, y, width, height)
    float2 src_full;   // full source dimensions in pixels
};

[[vk::push_constant]]
CameraCompositePC cameraPC;

// BT.601 limited-range YUV -> RGB. Inputs are normalized [0,1] values.
float3 yuv2rgb(float y, float u, float v) {
    float yn = y * 255.0;
    float un = u * 255.0 - 128.0;
    float vn = v * 255.0 - 128.0;
    float r = 1.164383 * (yn - 16.0) + 1.596027 * vn;
    float g = 1.164383 * (yn - 16.0) - 0.391762 * un - 0.812968 * vn;
    float b = 1.164383 * (yn - 16.0) + 2.017232 * un;
    return saturate(float3(r, g, b) / 255.0);
}

[shader("fragment")]
float4 main(VSOutput input) : SV_TARGET {
    // Map the viewport uv onto the source crop region.
    float2 src_origin = cameraPC.src_rect.xy / cameraPC.src_full;
    float2 src_extent = cameraPC.src_rect.zw / cameraPC.src_full;
    float2 suv = src_origin + input.uv * src_extent;
    if ((cameraPC.flags & 1u) != 0u) suv.x = 1.0 - suv.x;
    if ((cameraPC.flags & 2u) != 0u) suv.y = 1.0 - suv.y;

    float3 rgb;
    if (cameraPC.format == 2u) {
        // NV12: Y plane (R8) + interleaved UV plane (RG8), chroma at half res.
        float y = allTextures[NonUniformResourceIndex(cameraPC.src_slot0)]
            .Sample(cameraSampler, suv).r;
        float2 uv_t = suv;
        float2 uv_val = allTextures[NonUniformResourceIndex(cameraPC.src_slot1)]
            .Sample(cameraSampler, uv_t).rg;
        rgb = yuv2rgb(y, uv_val.r, uv_val.g);
    } else if (cameraPC.format == 1u) {
        // YUY2 packed 4:2:2 in an R8G8B8A8 image of half width.
        float pixel_col = suv.x * cameraPC.src_full.x;         // source pixel column
        float pair_index = floor(pixel_col * 0.5);
        float2 tuv = float2((pair_index + 0.5) / (cameraPC.src_full.x * 0.5), suv.y);
        float4 t = allTextures[NonUniformResourceIndex(cameraPC.src_slot0)]
            .Sample(cameraSampler, tuv);
        float pair_pos = frac(pixel_col * 0.5);
        float y = (pair_pos < 0.5) ? t.r : t.b;
        rgb = yuv2rgb(y, t.g, t.a);
    } else {
        // Xrgb8888 stored as B8G8R8A8 (SDL byte order), alpha ignored.
        float4 t = allTextures[NonUniformResourceIndex(cameraPC.src_slot0)]
            .Sample(cameraSampler, suv);
        rgb = t.bgr;
    }

    //rgb.x = 2.0;

    return float4(rgb, 1.0);
}
