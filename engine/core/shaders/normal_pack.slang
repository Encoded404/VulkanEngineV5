// GPU-side encoding for the packed 32-bit tangent frame, the exact mirror of
// engine/core/assets/NormalEncoding.cppm. Bit layout (unchanged — see
// normal_encoding.slang for the decoder):
//   [31]     handedness (1 = -1.0)
//   [30..21] tangent diamond d (10 bits)
//   [19..10] octahedral normal y (10 bits)
//   [9..0]   octahedral normal x (10 bits)
//   [20]     spare
//
// This file is deliberately self-contained (no include of
// normal_encoding.slang): the modifier stack includes both headers, and a
// PackTBN that also pulled in the decoder would redefine its functions. The
// encode-side helpers therefore carry a Pack prefix, and the one decoder call
// the packer needs (the requantized normal) has its own local implementation.
//
// Change this file and NormalEncoding.cppm together; the CPU/GPU parity test
// pins them to each other.

static const uint kPackOctBits = 10;
static const uint kPackOctMax = (1u << kPackOctBits) - 1u;
static const uint kPackDiamondBits = 10;
static const uint kPackDiamondMax = (1u << kPackDiamondBits) - 1u;

// ── Signed octahedral encode (seam-free wrap, Cigolle et al. JCGT 2014) ──
float2 PackOctahedralEncode(float3 n) {
    float absSum = abs(n.x) + abs(n.y) + abs(n.z);
    float2 o = float2(n.x / absSum, n.y / absSum);
    if (n.z < 0.0) {
        float wx = (1.0 - abs(o.y)) * (o.x >= 0.0 ? 1.0 : -1.0);
        float wy = (1.0 - abs(o.x)) * (o.y >= 0.0 ? 1.0 : -1.0);
        o = float2(wx, wy);
    }
    return o;
}

// Quantize the 2D octahedral coords to kPackOctBits per component (UNORM).
uint PackQuantizeOct(float2 o) {
    float maxVal = float(kPackOctMax);
    float remap = maxVal * 0.5;
    uint qx = uint(clamp(o.x * remap + remap, 0.0, maxVal));
    uint qy = uint(clamp(o.y * remap + remap, 0.0, maxVal));
    return (qy << kPackOctBits) | qx;
}

float2 PackDequantizeOct(uint bits) {
    float maxVal = float(kPackOctMax);
    float remap = 1.0 / maxVal;
    return float2(float(bits & kPackOctMax) * remap * 2.0 - 1.0,
                  float((bits >> kPackOctBits) & kPackOctMax) * remap * 2.0 - 1.0);
}

// The requantized normal for an octahedral code. The tangent basis is built
// from THIS vector, not the input normal: otherwise quantization noise can flip
// the basis region and the reconstructed tangent points somewhere unrelated.
//
// Deliberately NOT normalized: NormalEncoding::OctahedralDecode (the encoder's
// own requantization step, which this must mirror bit-for-bit) returns the
// folded vector as-is. The shader-side decoder in normal_encoding.slang does
// normalize, but that one consumes the finished word, not this intermediate.
float3 PackDecodeOctahedral(float2 f) {
    float3 n = float3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    float t = saturate(-n.z);
    n.xy += n.xy >= 0.0 ? -t : t;
    return n;
}

// ── Duff/Frisvad orthonormal basis (JCGT 2017) ──
void PackDuffBasis(float3 n, out float3 t, out float3 b) {
    float sign = n.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (sign + n.z);
    float c = n.x * n.y * a;
    t = float3(1.0 + sign * n.x * n.x * a, sign * c, -sign * n.x);
    b = float3(c, sign + n.y * n.y * a, -n.y);
}

// ── Diamond encode (Ong 2023) ──
uint PackQuantizeDiamond(float2 dir) {
    float denom = abs(dir.x) + abs(dir.y);
    float2 d = float2(0.0, 0.0);
    if (denom > 1e-20) {
        d = float2(dir.x / denom, dir.y / denom);
    }
    float2 s = d * 0.5 + 0.5;
    float maxVal = float(kPackDiamondMax);
    uint qx = uint(clamp(s.x * maxVal, 0.0, maxVal));
    uint qy = uint(clamp(s.y * maxVal, 0.0, maxVal));
    return (qy << kPackDiamondBits) | qx;
}

// Pack normal + tangent + handedness into one uint32. Mirrors
// NormalEncoding::PackTBN exactly: the tangent is orthogonalized against the
// requantized normal, so a non-orthogonal input cannot flip the basis region.
uint PackTBN(float3 normal, float3 tangent, float handedness) {
    // 1. Signed-octahedral quantize the normal.
    uint octBits = PackQuantizeOct(PackOctahedralEncode(normal));

    // 2. Build the basis from the requantized normal, not the input normal.
    float3 nQ = PackDecodeOctahedral(PackDequantizeOct(octBits));
    float3 bT, bB;
    PackDuffBasis(nQ, bT, bB);

    // 3. Orthogonalize the tangent against the requantized normal and project
    //    it into the 2D basis plane.
    float dotNT = dot(normal, nQ);
    float3 tOrtho = tangent - nQ * dotNT;
    float tLen = length(tOrtho);
    if (tLen < 1e-8) {
        // Degenerate tangent (parallel to normal): the basis tangent is a valid
        // orthonormal choice.
        tOrtho = bT;
        tLen = 1.0;
    }
    tOrtho /= tLen;
    float2 dir = float2(dot(tOrtho, bT), dot(tOrtho, bB));

    // 4. Diamond-quantize the 2D tangent direction.
    uint dBits = PackQuantizeDiamond(dir);

    // 5. Assemble. Bit 20 (between oct y and diamond) stays spare.
    uint signBit = handedness < 0.0 ? 1u : 0u;
    return (signBit << 31) | ((dBits & 0x3FFu) << 21) | (octBits & 0xFFFFFu);
}
