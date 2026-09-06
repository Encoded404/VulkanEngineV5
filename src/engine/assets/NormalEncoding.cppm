module;

export module VulkanEngine.Mesh.NormalEncoding;

import std;

import VulkanEngine.Mesh.MeshTypes;

export namespace VulkanEngine::NormalEncoding {

// ─────────────────────────────────────────────────────────────────────────────
// Packed tangent frame: 32 bits, layout [31]=handedness, [30..21]=diamond d,
// [19..10]=octahedral y, [9..0]=octahedral x.
//
//   Normal:  signed octahedral, 10+10 bits (John White, "Signed Octahedron
//            Normal Encoding", 2017). The seam-free wrap (Cigolle et al.,
//            JCGT 2014) makes the z-sign recoverable from the coordinate
//            region, so no separate sign bit is stored.
//   Tangent: rotation within a Frisvad/Duff orthonormal basis of the normal
//            (Duff et al., JCGT 2017), encoded with the diamond mapping
//            (Jeremy Ong, "Tangent Spaces and Diamond Encoding", 2023) —
//            trig-free decode.
//   Bit 20 is spare (reserved for a future zero-length-tangent sentinel).
//
// Decoding mirror lives in src/engine/shaders/normal_encoding.slang. The two
// MUST be changed together. Error characteristics (Kapoulkine, "Quantizing
// Tangent Frames", 2026): ~0.04° avg / 0.14° max normal error,
// ~0.09° avg / 0.24° max tangent error.
// ─────────────────────────────────────────────────────────────────────────────

inline constexpr std::uint32_t kOctBits = 10;
inline constexpr std::uint32_t kDiamondBits = 10;

// Local 2D vector type (MeshVertexVec2 carries u/v naming that doesn't fit
// direction math here).
struct Vec2 {
    float x, y; //NOLINT(misc-non-private-member-variables-in-classes)
};

// ── Signed octahedral mapping ──

// Project unit vector onto octahedron, fold to 2D square in [-1,1].
// Uses the seam-free wrap from Cigolle et al. (JCGT 2014) — components AND
// signs are swapped together so there is no discontinuity artifact at the
// z-sign boundary.
[[nodiscard]] inline Vec2 OctahedralEncode(const MeshVertexVec3& n) {
    const float abs_sum = std::abs(n.x) + std::abs(n.y) + std::abs(n.z);
    Vec2 o{n.x / abs_sum, n.y / abs_sum};
    if (n.z < 0.0f) {
        const float wx = (1.0f - std::abs(o.y)) * (o.x >= 0.0f ? 1.0f : -1.0f);
        const float wy = (1.0f - std::abs(o.x)) * (o.y >= 0.0f ? 1.0f : -1.0f);
        o.x = wx;
        o.y = wy;
    }
    return o;
}

// Inverse of OctahedralEncode. Returns a unit vector (Stubbe decode).
[[nodiscard]] inline MeshVertexVec3 OctahedralDecode(const Vec2& o) {
    MeshVertexVec3 n{o.x, o.y, 1.0f - std::abs(o.x) - std::abs(o.y)};
    const float t = std::max(-n.z, 0.0f);
    n.x += n.x >= 0.0f ? -t : t;
    n.y += n.y >= 0.0f ? -t : t;
    return n;
}

// Quantize the 2D octahedral coords to kOctBits bits per component (UNORM).
[[nodiscard]] inline std::uint32_t QuantizeOct(const Vec2& o) {
    const float max_val = static_cast<float>((1u << kOctBits) - 1);
    const float remap = max_val * 0.5f;
    // [-1,1] -> [0, 2^bits-1]
    auto q = [&](float v) {
        return static_cast<std::uint32_t>(std::clamp(v * remap + remap, 0.0f, max_val));
    };
    return (q(o.y) << kOctBits) | q(o.x);
}

[[nodiscard]] inline Vec2 DequantizeOct(std::uint32_t bits) {
    const float max_val = static_cast<float>((1u << kOctBits) - 1);
    const float remap = 1.0f / max_val;
    auto q = [&](std::uint32_t v) { return static_cast<float>(v) * remap * 2.0f - 1.0f; };
    return {q(bits & ((1u << kOctBits) - 1)), q((bits >> kOctBits) & ((1u << kOctBits) - 1))};
}

// ── Frisvad/Duff orthonormal basis (Duff et al., JCGT 2017) ──
// Given n (unit), produces orthonormal (t, b). Single discontinuity at
// n.z == -1, handled exactly as published.
inline void DuffBasis(const MeshVertexVec3& n, MeshVertexVec3& out_t, MeshVertexVec3& out_b) {
    const float sign = n.z >= 0.0f ? 1.0f : -1.0f;
    const float a = -1.0f / (sign + n.z);
    const float c = n.x * n.y * a;
    out_t = {1.0f + sign * n.x * n.x * a, sign * c, -sign * n.x};
    out_b = {c, sign + n.y * n.y * a, -n.y};
}

// ── Diamond mapping (Ong 2023) ──
// Encode a unit 2D direction onto the unit diamond |x|+|y| <= 1, then to
// [0,1]² UNORM with kDiamondBits per component.
[[nodiscard]] inline std::uint32_t QuantizeDiamond(const Vec2& dir) {
    // Project unit circle direction onto the diamond.
    const float denom = std::abs(dir.x) + std::abs(dir.y);
    float dx = 0.0f, dy = 0.0f;
    if (denom > 1e-20f) {
        dx = dir.x / denom;
        dy = dir.y / denom;
    }
    // Diamond [-1,1]² (|x|+|y|<=1) -> square [0,1]².
    const float sx = dx * 0.5f + 0.5f;
    const float sy = dy * 0.5f + 0.5f;
    const float max_val = static_cast<float>((1u << kDiamondBits) - 1);
    auto q = [&](float v) {
        return static_cast<std::uint32_t>(std::clamp(v * max_val, 0.0f, max_val));
    };
    return (q(sy) << kDiamondBits) | q(sx);
}

[[nodiscard]] inline Vec2 DequantizeDiamond(std::uint32_t bits) {
    const float max_val = static_cast<float>((1u << kDiamondBits) - 1);
    const float inv = 1.0f / max_val;
    auto q = [&](std::uint32_t v) { return static_cast<float>(v) * inv; };
    // Square [0,1]² -> diamond coords -> renormalize onto unit circle.
    float dx = q(bits & ((1u << kDiamondBits) - 1)) * 2.0f - 1.0f;
    float dy = q((bits >> kDiamondBits) & ((1u << kDiamondBits) - 1)) * 2.0f - 1.0f;
    const float norm = std::sqrt(dx * dx + dy * dy);
    if (norm > 1e-20f) {
        dx /= norm;
        dy /= norm;
    } else {
        dx = 1.0f;
        dy = 0.0f;
    }
    return {dx, dy};
}

// ── Public packing API ──

// Round-trip helper: decode the octahedral normal that QuantizeOct would
// store. The tangent basis MUST be built from this (the decoder's) normal,
// not the original — otherwise quantization noise can flip the basis region
// and the reconstructed tangent points in a wildly different direction.
[[nodiscard]] inline MeshVertexVec3 RequantizedNormal(const MeshVertexVec3& n) {
    return OctahedralDecode(DequantizeOct(QuantizeOct(OctahedralEncode(n))));
}

// Pack normal + tangent + handedness into one uint32.
// Tangent need not be orthogonal to the normal (it is orthogonalized here
// against the requantized normal, matching the shader-side reconstruction).
[[nodiscard]] inline std::uint32_t PackTBN(const MeshVertexVec3& normal,
                                           const MeshVertexVec3& tangent,
                                           const float handedness) {
    // 1. Signed-octahedral quantize the normal.
    const std::uint32_t oct_bits = QuantizeOct(OctahedralEncode(normal));

    // 2. Build the basis from the requantized normal — not the input normal.
    const MeshVertexVec3 n_q = OctahedralDecode(DequantizeOct(oct_bits));
    MeshVertexVec3 b_t, b_b;
    DuffBasis(n_q, b_t, b_b);

    // 3. Orthogonalize the tangent against the requantized normal, project
    //    into the 2D basis plane.
    const float dot_nt = normal.x * n_q.x + normal.y * n_q.y + normal.z * n_q.z;
    MeshVertexVec3 t_ortho{
        tangent.x - n_q.x * dot_nt,
        tangent.y - n_q.y * dot_nt,
        tangent.z - n_q.z * dot_nt,
    };
    float t_len = std::sqrt(t_ortho.x * t_ortho.x + t_ortho.y * t_ortho.y + t_ortho.z * t_ortho.z);
    if (t_len < 1e-8f) {
        // Degenerate tangent (parallel to normal): fall back to the basis
        // tangent itself, which is a valid orthonormal choice.
        t_ortho = b_t;
        t_len = 1.0f;
    }
    t_ortho.x /= t_len; t_ortho.y /= t_len; t_ortho.z /= t_len;
    const Vec2 dir{
        t_ortho.x * b_t.x + t_ortho.y * b_t.y + t_ortho.z * b_t.z,
        t_ortho.x * b_b.x + t_ortho.y * b_b.y + t_ortho.z * b_b.z,
    };

    // 4. Diamond-quantize the 2D tangent direction.
    const std::uint32_t d_bits = QuantizeDiamond(dir);

    // 5. Assemble. Note bit 20 (between oct y and diamond) is spare.
    const std::uint32_t sign_bit = handedness < 0.0f ? 1u : 0u;
    return (sign_bit << 31) | ((d_bits & 0x3FFu) << 21) | (oct_bits & 0xFFFFFu);
}

} // namespace VulkanEngine::NormalEncoding
