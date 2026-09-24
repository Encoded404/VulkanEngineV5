# Normals, tangents & packed tangent frames

This document records the encoding decisions and measured error data for surface
normal and tangent storage, and credits the techniques and their authors.

---

## 1. Decision: pack the whole tangent frame into 32 bits

A vertex carries position, one 32-bit packed tangent frame, and texcoord. There is
no separate normal (12 B) or tangent (16 B) attribute — the entire tangent frame is
recovered from 4 bytes by a ~40-instruction branchless decode in the vertex shader.

On a vertex-bandwidth-bound pipeline this is the dominant win; the decode ALU cost
is noise on any modern GPU.

## 2. Packed tangent frame layout (32 bits)

```
bit  31        handedness (+1 / -1) for the reconstructed bitangent
bits 30..21    tangent direction, diamond encoding, 10 bits
bit  20        spare (reserved, e.g. zero-length-tangent sentinel)
bits 19..10    octahedral normal y, 10 bits
bits  9..0     octahedral normal x, 10 bits
```

### 2.1 Normal: octahedral mapping

The unit sphere is projected onto an octahedron and unfolded into a 2D square.
Origin:

- Quílez, *Sphere Mapping* discussion (2000s) and Meyer et al. (2010), who
  introduced floating-point octahedral normal vectors:
  Q. Meyer, J. Süßmuth, G. Sußner, M. Stamminger, G. Greiner,
  **"On Floating-Point Normal Vectors"**, *Computer Graphics Forum* 29(6),
  2010. <https://lgdv.cs.fau.de/get/1602>

Quantized variants and a rigorous comparison against XYZ/SNORM/spherical/
Fibonacci alternatives:

- Z. H. Cigolle, S. Donow, D. Evangelakos, M. Mara, M. McGuire, Q. Meyer,
  **"A Survey of Efficient Representations for Independent Unit Vectors"**,
  *Journal of Computer Graphics Techniques* 3(2), 2014.
  <https://jcgt.org/published/0003/02/01/>
  — the survey's conclusion (best quality/cost of any 2-component encoding)
  and its **seam-free wrap** (components *and* signs swapped together, so no
  discontinuity artifact at the z-sign boundary) are both used here.

A practical implementation writeup with visual comparisons:

- K. Narkowicz, **"Octahedron Normal Vector Encoding"**, 2014.
  <https://knarkowicz.wordpress.com/2014/04/16/octahedron-normal-vector-encoding/>

The GPU decode function is **Rune Stubbe's branchless variant** (2017), which
replaces the older reconstruct-and-branch approach:

- R. Stubbe, branchless octahedron decode, 2017.
  <https://twitter.com/Stubbesaurus/status/937994790553227264>
  (as referenced by the Narkowicz post above)

An optional variant used by the bit-budget math: signed octahedral encoding
frees the sign into the coordinate region for odd bit counts.

- J. White, **"Signed Octahedron Normal Encoding"**, 2017.
  <https://johnwhite3d.blogspot.com/2017/10/signed-octahedron-normal-encoding.html>

### 2.2 Tangent: diamond encoding within a Frisvad/Duff basis

Storing a full second vector (the tangent) would double the cost. Instead, the
tangent is expressed as a 2D rotation within an orthonormal basis built from
the normal, and that direction is quantized with the diamond mapping —
octahedral-style precision, but for a tangent-plane direction, with a
**trig-free** decode (unlike angle/sincos schemes).

Basis construction from the normal:

- T. Duff, J. Burgess, P. Christensen, C. Hery, A. Kensler, M. Liani,
  R. Villemasin, **"Building an Orthonormal Basis, Revisited"**,
  *Journal of Computer Graphics Techniques* 6(1), 2017.
  <https://jcgt.org/published/0006/01/01/>
  (branchless, single discontinuity at n.z = −1)

Tangent-as-scalar encoding alternatives, and the diamond method itself:

- J. Ong, **"Tangent Spaces and Diamond Encoding"**, 2023.
  <https://www.jeremyong.com/graphics/2023/01/09/tangent-spaces-and-diamond-encoding/>

A production precedent for scalar tangent storage (angle form, 3 bytes/vertex):

- G. Wronski et al., **"Rendering the Hellscape of Doom Eternal"**,
  SIGGRAPH 2020, *Advances in Real-Time Rendering in Games*, slide 35.
  <https://advances.realtimerendering.com/s2020/RenderingDoomEternal.pdf>

### 2.3 Measured error budgets

The 10-bit budgets were chosen from A. Kapoulkine's measurements of these
exact encodings over real assets:

- A. Kapoulkine (zeux), **"Quantizing Tangent Frames"**, 2026.
  <https://zeux.io/2026/04/30/quantizing-tangent-frames/>

Representative numbers from that analysis: a 32-bit
oct10×2s + diamond10 tangent frame achieves ≈ 0.04° average / 0.14° maximum
normal error and ≈ 0.09°–0.17° average / 0.24°–0.45° maximum tangent error —
below the quantization noise of a BC5 normal map, so the vertex format never
becomes the precision bottleneck. For comparison, smallest-three quaternion
packing (3×10+2) lands at ≈ 0.06° avg / 0.20° max for *both* vectors, while
plain SNORM8 per-axis is ≈ 0.9° max.

## 3. Encoding rules

1. **The tangent basis is built from the *requantized* normal** — the
   octahedral value is encoded, dequantized, and *that* normal's basis is used
   to project the tangent. Using the pre-quantization normal lets quantization
   noise flip the Duff basis region and produce a wildly wrong tangent.
   (Same trap identified by Kapoulkine for angle encoding.)
2. The tangent is orthogonalized against the requantized normal before
   projection; a degenerate (zero-length) tangent falls back to the basis
   tangent.
3. Always renormalize after decode. Octahedral decode output is renormalized
   in-shader; hides quantization drift at negligible cost.

## 4. Decision: MikkTSpace for tangent generation

Normal maps in the wild are baked in **MikkTSpace** tangent space (xNormal,
Substance, Blender, etc.). Computing tangents with a different convention
renders third-party normal maps subtly-to-visibly wrong, especially at hard
edges and mirrored UVs. The engine therefore uses the canonical reference
implementation:

- M. S. Mikkelsen, **"Tangent Space Computation for Arbitrary Meshes"**
  (MikkTSpace), 2008/2011.
  Reference implementation: <https://github.com/mmikk/MikkTSpace> (zlib-style
  license).
- Mikkelsen's degenerate-triangle (zero UV area) rule is the non-obvious part
  that hand-rolled implementations get wrong; the reference handles it, and
  glTF 2.0 normatively references MikkTSpace as the fallback when the
  `TANGENT` attribute is absent.

Generation rules:

- Run MikkTSpace per submesh **after** vertex dedup (MikkTSpace accumulates
  across shared vertices; true UV seams must already be split into distinct
  vertices — a `(pos, normal, uv)` dedup key guarantees this).
- Prefer the file's baked `TANGENT` attribute (including `.w` handedness) and
  only generate when it is absent.

Terminology note: the third basis vector is the **bitangent** (per
T. Forsyth, *"Bitangent versus Binormal"*,
<https://terathon.com/blog/tangent-space/>); it is never stored — it is
reconstructed in the fragment shader as `cross(N, T) * handedness`.

## 5. Transforming normals: the normal matrix

A normal is transformed by the **inverse-transpose** of the model matrix, not
the matrix itself. `mul(n, modelMatrix)` is only correct under uniform scale
(when the model matrix has no shear, the uniform factor is erased by the
shader's `normalize()`).

### Closed form for a rotation-scale transform

For a transform composed as `modelMat(pos, rot, scale) = T · R · S` with an
orthonormal quaternion rotation `R` and diagonal scale `S`:

```
(M⁻¹)ᵀ = (R · S)⁻ᵀ = R · S⁻¹
```

No matrix inversion is needed — the closed form is built analytically from the
same quaternion code path as the model matrix, with `1/scale` per row and a
guard against degenerate scale components.

### Why per-instance, not per-vertex

The normal matrix is a function of the transform, not the vertex — computing
it per vertex would repeat identical work for every vertex of an instance. The
invariant: **the normal matrix rides with the transform entry, computed by
whoever composes the transform**.

### Consistency

The model-view-projection matrix, model matrix, and normal matrix must be
written from the same transform snapshot in the same invocation. Mixed
snapshots show up as lighting that lags geometry on fast rotation.

Negative scale (mirrored instances) flips the determinant — handle winding/
culling separately; the normal matrix math itself remains valid.

## 6. Fragment-stage normal mapping

The world-space TBN is built from the interpolated geometric normal (so the
frame stays orthonormal even where the interpolated tangent drifts from
perpendicularity). The normal map is sampled with the glTF convention
(`n_ts = tex.rgb * 2 − 1`, green up) and applied only where a normal texture
exists.

## 7. Alternative encodings considered (and why not)

| Encoding | Size | Verdict |
|---|---|---|
| Per-axis SNORM8/16 (glTF KHR_mesh_quantization style) | 6–8 B | correct but wasteful; SNORM8 visibly wobbles on specular |
| Per-axis SNORM10 (RGB10A2-style) | 8 B | excellent quality, cannot fit both vectors in 4 B |
| Smallest-three quaternion (QTangent, Crytek) | 4 B | symmetric error, branchless decode, but worse normal precision and no bit rebalancing; revisit for visibility-buffer pipelines (nlerp of quaternion frames) |
| Tangent *angle* encoding (DOOM Eternal) | ≤4 B | needs sincos decode; diamond achieves the same shape trig-free |
| Spherical coordinates | 2–4 B | non-uniform distribution + trig; strictly dominated by octahedral |
| Best-fit normals (Crytek LUT) | 12 B | 3 components anyway; obsolete |

Full quantitative comparison (all numbers above): Kapoulkine 2026, Cigolle
et al. 2014.

## 8. Reference list

1. Meyer, Süßmuth, Sußner, Stamminger, Greiner — *On Floating-Point Normal
   Vectors*, CGF 2010.
2. Cigolle, Donow, Evangelakos, Mara, McGuire, Meyer — *A Survey of Efficient
   Representations for Independent Unit Vectors*, JCGT 3(2), 2014.
3. Narkowicz — *Octahedron Normal Vector Encoding*, 2014.
4. Stubbe — branchless octahedral decode, 2017.
5. White — *Signed Octahedron Normal Encoding*, 2017.
6. Duff, Burgess, Christensen, Hery, Kensler, Liani, Villemasin — *Building an
   Orthonormal Basis, Revisited*, JCGT 6(1), 2017.
7. Ong — *Tangent Spaces and Diamond Encoding*, 2023.
8. Kapoulkine — *Quantizing Tangent Frames*, 2026; meshoptimizer
   (`meshopt_encodeFilterOct`) reference implementation.
9. Wronski et al. — *Rendering the Hellscape of Doom Eternal*, SIGGRAPH 2020.
10. Mikkelsen — *Tangent Space Computation for Arbitrary Meshes* (MikkTSpace),
    2008/2011; reference implementation.
11. Kaplanyan — *CryENGINE 3: Reaching the Speed of Light*, SIGGRAPH 2010
    (QTangent context).
12. Forsyth — *Bitangent versus Binormal*, Terathon blog.
