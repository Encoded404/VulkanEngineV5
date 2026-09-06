// Shared GPU-side decoding for the packed 32-bit tangent frame.
// Packed layout (see src/engine/assets/NormalEncoding.cppm for the encoder):
//   [31]    handedness (1 = -1.0)
//   [30..21] tangent diamond d (10 bits)
//   [19..10] octahedral normal y (10 bits)
//   [9..0]  octahedral normal x (10 bits)
//   [20]    spare
//
// Techniques, in order of appearance:
//   - Signed octahedral normal encoding: John White, "Signed Octahedron
//     Normal Encoding" (2017); octahedral mapping: Meyer et al., "On
//     Floating-Point Normal Vectors" (CGF 2010); seam-free wrap: Cigolle et
//     al., "A Survey of Efficient Representations for Independent Unit
//     Vectors" (JCGT 2014); branchless decode: Rune Stubbe (2017).
//   - Orthonormal basis from a normal: Duff et al., "Building an Orthonormal
//     Basis, Revisited" (JCGT 2017).
//   - Diamond tangent encoding: Jeremy Ong, "Tangent Spaces and Diamond
//     Encoding" (2023).
//
// Change this file and NormalEncoding.cppm together.

static const uint kOctBitsEnc = 10;
static const float kOctMaxEnc = 1023.0;
static const uint kDiamondBitsEnc = 10;
static const float kDiamondMaxEnc = 1023.0;

// ── Octahedral decode (Stubbe's branchless version) ──
float3 DecodeOctahedral(float2 f) {
    float3 n = float3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    float t = saturate(-n.z);
    n.xy += n.xy >= 0.0 ? -t : t;
    return normalize(n);
}

// Dequantize 20 bits of octahedral data into a 2D coord in [-1,1]² and
// decode. The encoder's seam-free wrap (Cigolle et al.) guarantees that the
// z >= 0 hemisphere maps to |x|+|y| <= 1 and the z < 0 hemisphere folds into
// the wrapped corners, so no explicit sign bit is needed — the region of the
// coordinate carries the sign.
float3 DecodePackedOctahedral(uint bits) {
    float2 f;
    f.x = (float(bits & 0x3FFu) / kOctMaxEnc) * 2.0 - 1.0;
    f.y = (float((bits >> kOctBitsEnc) & 0x3FFu) / kOctMaxEnc) * 2.0 - 1.0;
    return DecodeOctahedral(f);
}

// ── Duff/Frisvad orthonormal basis ──
void BuildOrthonormalBasis(float3 n, out float3 t, out float3 b) {
    float sign = n.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (sign + n.z);
    float c = n.x * n.y * a;
    t = float3(1.0 + sign * n.x * n.x * a, sign * c, -sign * n.x);
    b = float3(c, sign + n.y * n.y * a, -n.y);
}

// ── Tangent diamond decode ──
// Returns the unit 2D tangent direction within the normal's (t, b) basis.
float2 DecodePackedDiamond(uint bits) {
    float2 d;
    d.x = (float(bits & 0x3FFu) / kDiamondMaxEnc) * 2.0 - 1.0;
    d.y = (float((bits >> kDiamondBitsEnc) & 0x3FFu) / kDiamondMaxEnc) * 2.0 - 1.0;
    // Diamond coords have |x|+|y| <= 1; renormalize onto the unit circle.
    return normalize(d);
}

// ── Full packed tangent frame unpack ──
// Returns: xyz = model-space tangent (unit), w = handedness (+1/-1).
// The decoded normal is written to outNormal.
float4 UnpackTBN(uint packed, out float3 outNormal) {
    outNormal = DecodePackedOctahedral(packed & 0xFFFFFu);

    float3 bT, bB;
    BuildOrthonormalBasis(outNormal, bT, bB);

    float2 dir = DecodePackedDiamond((packed >> 21) & 0x3FFu);
    float3 tangent = normalize(bT * dir.x + bB * dir.y);

    float handedness = (packed & (1u << 31)) != 0 ? -1.0 : 1.0;
    return float4(tangent, handedness);
}
