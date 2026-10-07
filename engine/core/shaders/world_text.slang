// Depth-tested world-space text.
//
// One quad per glyph, positioned in world space and transformed by the scene
// camera, sampling the MSDF atlas page from the engine bindless array (set 0,
// binding 0) exactly the way the standard mesh and unlit shaders sample
// materials. There is no vertex buffer: the quad corner comes from SV_VertexID
// and all per-glyph data from the instance storage buffer, which is the engine's
// vertex-pulling convention.
//
// The instance buffer is the pass's own set 5 binding 0, one buffer for the whole
// draw rather than a per-material array, because a world-text pass batches every
// glyph it draws in a frame into a single buffer.
//
// The atlas page is RGBA8_UNORM, sampled as *linear* (never sRGB: a distance
// field read through an sRGB view is a different field). rgb holds the three
// multi-channel distance channels and a holds the true single-channel signed
// distance. Coverage is reconstructed the standard MSDF way: the median of the
// three channels is the signed distance, and the derivative of the texture
// coordinate converts it to a screen-pixel distance so the edge stays one pixel
// wide at any world size or distance. The alpha channel is not decoration: where
// the multichannel median flattens (corners, and a glyph drawn smaller than its
// field) the true distance has the larger magnitude gradient, so taking the
// distance with the larger magnitude keeps the edge sharp instead of letting the
// reconstruction wash out. The pass's px_range is exactly the range the field
// was generated with, in atlas texels, so the two cannot disagree.
//
// Blending is straight-alpha source-over, the same composite the screen-space
// text overlay uses; depth test is on (less-or-equal, the engine's depth 0..1
// convention) and depth write is off, so scene geometry occludes world text and
// the text never hides geometry a later pass would draw.

[[vk::binding(0, 0)]]
Sampler2D<float4> allTextures[];

// One world-space glyph quad. C layout, 80 bytes; the C++ mirror is
// VulkanEngine::SceneRenderer::WorldTextInstance. Change both together.
struct WorldTextInstance {
    float3 origin;   // world position of the uv (0,0) corner
    float3 right;    // world-space vector to the uv (1,0) corner
    float3 up;       // world-space vector to the uv (0,1) corner
    float2 uv_min;   // inner (field) region of the atlas page, normalised
    float2 uv_max;
    float4 color;    // straight alpha
    float px_range;  // distance range the field was generated with, in texels
    uint page;       // bindless slot of the atlas page
    uint pad0;
};

[[vk::binding(0, 5)]]
StructuredBuffer<WorldTextInstance, CDataLayout> worldTextInstances;

// Camera and atlas size. The matrix is the engine's row-vector convention: it is
// multiplied on the right of a row vector, mul(float4(p, 1), m).
struct WorldTextConstants {
    float4x4 view_proj;
    float2 atlas_size;
    float2 pad0;
};

[[vk::push_constant]]
WorldTextConstants constants;

struct VSOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : TEXCOORD1;
    nointerpolation float px_range : TEXCOORD2;
    nointerpolation uint page : TEXCOORD3;
};

// Two triangles, corner order (0,0) (1,0) (1,1) / (0,0) (1,1) (0,1). Corner y
// grows down the field because the field's row 0 is the glyph's top.
static const float2 kQuadCorners[6] = {
    float2(0.0, 0.0), float2(1.0, 0.0), float2(1.0, 1.0),
    float2(0.0, 0.0), float2(1.0, 1.0), float2(0.0, 1.0),
};

static float median3(float3 v) {
    return max(min(v.r, v.g), min(max(v.r, v.g), v.b));
}

[shader("vertex")]
VSOutput vs_main(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID) {
    const WorldTextInstance instance = worldTextInstances[instance_id];
    const float2 corner = kQuadCorners[vertex_id];
    const float3 world = instance.origin + corner.x * instance.right + corner.y * instance.up;

    VSOutput output;
    output.position = mul(float4(world, 1.0), constants.view_proj);
    output.uv = lerp(instance.uv_min, instance.uv_max, corner);
    output.color = instance.color;
    output.px_range = instance.px_range;
    output.page = instance.page;
    return output;
}

[shader("fragment")]
float4 ps_main(VSOutput input) : SV_TARGET {
    const float4 sample = allTextures[NonUniformResourceIndex(input.page)].Sample(input.uv);

    // The distance range in atlas texels maps to a fraction of the texture; the
    // texture-coordinate derivative says how many texels one screen pixel covers,
    // so the dot product is the conversion from normalized distance to screen
    // pixels. The 0.5 factor makes the field's range span one screen pixel on
    // either side of the edge, and the floor keeps a degenerate fwidth from
    // collapsing the edge.
    const float2 unit_range = float2(input.px_range, input.px_range) / constants.atlas_size;
    const float2 screen_tex_size = 1.0 / max(fwidth(input.uv), float2(1e-8, 1e-8));
    const float screen_px_range = max(0.5 * dot(unit_range, screen_tex_size), 1.0);

    // Median reconstruction, sharpened by the true distance in alpha: the
    // multichannel field resolves corners but its median flattens there, while
    // the true field keeps a straight gradient. Taking whichever signed distance
    // is further from the boundary keeps the edge crisp when a glyph is drawn
    // smaller than the field it was generated at.
    const float distance_rgb = median3(sample.rgb) - 0.5;
    const float distance_true = sample.a - 0.5;
    const float distance = abs(distance_true) > abs(distance_rgb) ? distance_true : distance_rgb;

    const float coverage = saturate(screen_px_range * distance + 0.5);
    return float4(input.color.rgb, input.color.a * coverage);
}
