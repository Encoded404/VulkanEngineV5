// Shared Hi-Z occlusion helpers, used by pre_cull and occlusion_cull. Ported
// from occlusion_cull.slang: exact for rolled views and scaled models,
// correct for perspective and orthographic.
//
// The helpers are parameterized instead of reading a push-constant block so
// this header carries no hidden push-constant contract.
//
// Depth convention (both call sites): non-reversed [0,1] (0 = near, 1 = far),
// every Hi-Z texel stores the FARTHEST depth in its footprint (max reduction
// in hiz_gen).
//
// This header is leaf-only (it includes nothing) and has no entry point: it is
// never compiled on its own and is not part of the _engine_shaders list, only
// of COMPILER_DEPENDS so every dependent shader rebuilds when it changes.

static const float kDepthEps = 0.001;

// Depth (NDC z) of the near pole of a sphere of radius worldR whose center
// projects to clipPos. projInfo must carry the projection's own scales:
//   .x = |projection x-scale|, .y = |projection y-scale|,
//   .z = projection z-scale,   .w = 1.0 if perspective else 0.0
// The near pole lies one radius closer to the camera along the view axis, so
// its clip-space z is clipPos.z + projInfo.z * worldR. For a perspective
// projection its w becomes clipPos.w - worldR; for an orthographic projection
// w stays 1 and no divide applies.
float NearPoleDepth(float4 clipPos, float worldR, float4 projInfo) {
    if (projInfo.w > 0.5)
        return (clipPos.z + projInfo.z * worldR) / max(clipPos.w - worldR, kDepthEps);
    return clipPos.z + projInfo.z * worldR;
}

// True when the near pole is at/beyond the near plane: the object is at least
// partially visible (possibly through the camera itself) and must never be
// culled. The near plane is z_clip + w_clip = 0 (non-reversed depth): for
// perspective w = -z_view so the near pole's clip z/w shift by projInfo.z and
// -worldR respectively; for ortho w stays 1 and only the z term applies (the
// near plane sits at z_ndc = -1). Zero-to-one depth: near plane is ndc z = 0,
// i.e. clip z = 0.
bool NearPoleClipped(float4 clipPos, float worldR, float4 projInfo) {
    return clipPos.z + projInfo.z * worldR <= kDepthEps;
}

// Largest valid Hi-Z mip index (hiz level 0 is half the depth resolution).
int MaxHizMip(int hizWidth, int hizHeight) {
    return int(floor(log2(max(float(hizWidth), float(hizHeight)))));
}

// Mip whose texels are ~1-2 level-0 texels large, so a single sample at that
// mip covers the whole rect (rectSizeUV in [0,1] UV units).
int RectMip(float2 rectSizeUV, int hizWidth, int hizHeight) {
    float maxDim = max(rectSizeUV.x * float(hizWidth), rectSizeUV.y * float(hizHeight));
    return min(int(ceil(log2(max(maxDim, 1.0)))), MaxHizMip(hizWidth, hizHeight));
}
