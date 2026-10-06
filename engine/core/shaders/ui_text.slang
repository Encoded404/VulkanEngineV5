// Screen-space text overlay.
//
// Draws one quad per glyph instance over the scene, sampling the glyph atlas
// page from the engine bindless array (set 0, binding 0) exactly the way the
// standard mesh and unlit shaders sample materials. There is no vertex buffer:
// the quad corner comes from SV_VertexID and all per-glyph data from the
// instance storage buffer, which is the engine's vertex-pulling convention.
//
// The instance buffer is the pass's own set 5 binding 0, one buffer for the
// whole draw rather than a per-material array, because a text pass batches every
// glyph it draws in a frame into a single buffer.
//
// The atlas page is R8_UNORM coverage, so the sampled value's .r channel is the
// glyph's alpha. The fragment emits straight (non-premultiplied) colour with
// that coverage folded into alpha, and the pipeline composites it with
// src-alpha / one-minus-src-alpha -- documented on TextPass::Setup. Coverage at
// the glyph edge is what makes the text antialiased, so the blend has to be a
// real source-over rather than an opaque write.

[[vk::binding(0, 0)]]
Sampler2D<float4> allTextures[];

// One glyph quad. C layout, 64 bytes; the C++ mirror is
// VulkanEngine::SceneRenderer::TextInstance. Change both together.
struct TextInstance {
    float2 position;   // screen-space pixel position of the quad's top-left
    float2 size;       // quad size in pixels (the glyph's ink box)
    float2 uv_min;     // inner (ink) region of the atlas page, normalised
    float2 uv_max;
    float4 color;      // straight alpha
    uint page;         // bindless slot of the atlas page
    uint pad0;
    uint pad1;
    uint pad2;
};

[[vk::binding(0, 5)]]
StructuredBuffer<TextInstance, CDataLayout> textInstances;

// Drawable area in pixels (8 bytes at offset 0, vertex stage).
[[vk::push_constant]]
float2 screen_size;

struct VSOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : TEXCOORD1;
    nointerpolation uint page : TEXCOORD2;
};

// Two triangles, corner order (0,0) (1,0) (1,1) / (0,0) (1,1) (0,1), so the
// interpolated uv follows the glyph's top-down bitmap orientation.
static const float2 kQuadCorners[6] = {
    float2(0.0, 0.0), float2(1.0, 0.0), float2(1.0, 1.0),
    float2(0.0, 0.0), float2(1.0, 1.0), float2(0.0, 1.0),
};

[shader("vertex")]
VSOutput vs_main(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID) {
    const TextInstance instance = textInstances[instance_id];
    const float2 corner = kQuadCorners[vertex_id];
    const float2 pixel = instance.position + corner * instance.size;

    VSOutput output;
    // Screen pixels to clip space. Vulkan's framebuffer origin is top-left and
    // clip-space +y already points down the framebuffer, so a y-down pixel
    // position maps straight through with no flip; only x needs the [-1,1]
    // remap of the viewport transform's (ndc + 1) / 2.
    output.position = float4(pixel.x / screen_size.x * 2.0 - 1.0,
                             pixel.y / screen_size.y * 2.0 - 1.0,
                             0.0, 1.0);
    output.uv = lerp(instance.uv_min, instance.uv_max, corner);
    output.color = instance.color;
    output.page = instance.page;
    return output;
}

[shader("fragment")]
float4 ps_main(VSOutput input) : SV_TARGET {
    // R8_UNORM coverage: the glyph's alpha is the red channel of the sample.
    const float coverage = allTextures[NonUniformResourceIndex(input.page)].Sample(input.uv).r;
    return float4(input.color.rgb, input.color.a * coverage);
}
