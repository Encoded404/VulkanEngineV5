// Slug (hb-gpu) world-space text: the vertex pulls the quad and the per-glyph
// blob offset from an instance storage buffer, and the fragment reconstructs
// coverage analytically from the glyph's encoded outline.
//
// This is the second backend behind the same world-text seam the MSDF path uses
// (WorldTextPass selects one at construction). The differences are deliberate:
// there is no atlas, no distance-field range and no uv rect. The quad is the
// glyph's *em box* in world space, the instance carries a flat offset into the
// blob storage buffer, and the fragment evaluates the Slug coverage function at
// the interpolated em-space sample point, so the glyph is rasterized at the
// resolution it is actually drawn at rather than at a fixed field size.
//
// The heavy lifting lives in slug_gpu.slang, which is the near-verbatim port of
// HarfBuzz's shared/draw shader sources with exactly two adaptations (the 8-byte
// atlas accessor and the row-vector dilation), both documented there and pinned
// by the device-free provenance test.
//
// The vertex contract follows what hb_gpu_dilate requires. Object space is the
// blob's own em space, so the quad's position and texcoord are the same
// design-unit coordinates and the em-to-object transform (and therefore its
// inverse, `jac`) is the identity; the em-to-clip matrix that carries the world
// placement, the requested size and the camera is built from the instance's
// world basis and the push constant's view-projection. The corner normal is the
// outward em-space diagonal, and the viewport size is what turns the computed
// dilation into half a screen pixel.

#include "slug_gpu.slang"

// One world-space Slug glyph quad. C layout, 76 bytes; the C++ mirror is
// VulkanEngine::SceneRenderer::SlugTextInstance. Change both together.
//
// `origin` is the world position of the em box's (em_min.x, em_min.y) corner,
// `right` the world vector from min_x to max_x and `up` the world vector from
// min_y to max_y, so any rotation or non-uniform scale of the entity turns the
// quad with it. `blob_offset` is the index of the glyph's first 8-byte unit in
// the blob storage buffer.
struct SlugTextInstance {
    float3 origin;
    float3 right;
    float3 up;
    float2 em_min;
    float2 em_max;
    float4 color;        // straight alpha
    uint blob_offset;    // blob buffer unit index
    uint pad0;
};

[[vk::binding(0, 5)]]
StructuredBuffer<SlugTextInstance, CDataLayout> slugTextInstances;

// Camera and viewport. The matrix is the engine's row-vector convention:
// multiplied on the right of a row vector, mul(float4(p, 1), m).
struct SlugTextConstants {
    float4x4 view_proj;
    float2 viewport;
    float2 pad0;
};

[[vk::push_constant]]
SlugTextConstants constants;

struct VSOutput {
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
    float4 color : TEXCOORD1;
    nointerpolation uint blob_offset : TEXCOORD2;
};

// Two triangles, corner order (0,0) (1,0) (1,1) / (0,0) (1,1) (0,1). Corner
// (0,0) is the em box's minimum corner, which in this y-up port is the glyph's
// lower left; corner y grows toward em_max, i.e. upward.
static const float2 kSlugQuadCorners[6] = {
    float2(0.0, 0.0), float2(1.0, 0.0), float2(1.0, 1.0),
    float2(0.0, 0.0), float2(1.0, 1.0), float2(0.0, 1.0),
};

[shader("vertex")]
VSOutput slug_vs_main(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID) {
    const SlugTextInstance instance = slugTextInstances[instance_id];
    const float2 corner = kSlugQuadCorners[vertex_id];

    // Object space is em space: the sample coordinate IS the position, so the
    // em-to-object transform is the identity and its inverse (`jac`) is too.
    float2 position = lerp(instance.em_min, instance.em_max, corner);
    float2 texcoord = position;

    // Outward em-space diagonal at this corner (em space is y-up, so the minimum
    // y corner points down). hb_gpu_dilate normalizes it.
    const float2 normal = float2(corner.x * 2.0 - 1.0, corner.y * 2.0 - 1.0);
    const float4 jac = float4(1.0, 0.0, 0.0, 1.0);

    // The em-to-world map, in the engine's row-vector form. `instance.origin` is
    // the world position of the box's *minimum* corner, so the affine part has
    // to subtract em_min before the per-em linear part is applied:
    //   world = (pos - em_min).x * ex + (pos - em_min).y * ey + origin
    // `right`/`up` span the em box, so dividing by the box size yields the world
    // vector of one em unit on each axis.
    const float2 em_size = max(instance.em_max - instance.em_min, float2(1.0 / 65536.0, 1.0 / 65536.0));
    const float3 ex = instance.right / em_size.x;
    const float3 ey = instance.up / em_size.y;
    const float3 translation = instance.origin - instance.em_min.x * ex - instance.em_min.y * ey;
    const float4x4 model = float4x4(
        float4(ex, 0.0),
        float4(ey, 0.0),
        float4(0.0, 0.0, 1.0, 0.0),
        float4(translation, 1.0));
    const float4x4 m = mul(model, constants.view_proj);

    hb_gpu_dilate(position, texcoord, normal, jac, m, constants.viewport);

    VSOutput output;
    output.position = mul(float4(position, 0.0, 1.0), m);
    output.texcoord = texcoord;
    output.color = instance.color;
    output.blob_offset = instance.blob_offset;
    return output;
}

[shader("fragment")]
float4 slug_ps_main(VSOutput input) : SV_TARGET {
    // Coverage in [0, 1] from the Slug analytic rasterizer. The colour is
    // straight alpha, composited by the pass's source-over blend, exactly like
    // the MSDF backend.
    const float coverage = hb_gpu_draw(input.texcoord, input.blob_offset);
    return float4(input.color.rgb, input.color.a * coverage);
}
