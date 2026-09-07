# Pre-Prepass Occlusion Culling: Analysis, Cost Model, and Design

*Design study for culling occluded objects **before** the depth prepass, using only current-frame information (no temporal / last-frame data).*

---

## Table of contents

1. [Motivation and problem statement](#1-motivation-and-problem-statement)
2. [Cost model of the current pipeline](#2-cost-model-of-the-current-pipeline)
3. [Theory: the two-sided conservativeness rule](#2-theory-the-occluderoccludee-asymmetry)
4. [Survey of existing techniques (with real measured data)](#3-existing-techniques-and-measured-data)
5. [Why OBB-as-occluder and pairwise OBB tests are out](#4-why-obb-vs-obb-pairwise-tests-are-out)
6. [Proposed design: occluder prepass + threshold selection](#5-proposed-design)
7. [Step-by-step cost estimates](#6-step-by-step-cost-estimates)
8. [Break-even analysis](#7-break-even-analysis)
9. [Pipelining and parallelism analysis](#8-pipelining-parallelism-and-serialization)
10. [Correctness analysis (conservativeness proof sketch)](#9-correctness)
11. [Alternatives considered and rejected](#10-alternatives-considered-and-rejected)
12. [Failure modes and debugging](#11-failure-modes)
13. [Implementation mapping to this engine](#12-implementation-mapping)
14. [References](#13-references)

---

## 1. Motivation and problem statement

The current frame layout (Renderer.cpp / SceneRendererFrame.cpp) is:

```
expand ──► depth prepass ──► hi-z gen ──► occlusion cull ──► collect ──► main pass
(all)      (ALL submeshes)   (from full    (cullEntries      (compact for
            via draw_count    depth         indexCount := 0)  main pass)
            buffer)           pyramid
```

The depth prepass draws **everything**: `expand.slang` accumulates every submesh's
`indexRange` into `draw_count_buffer`, and `DepthPrepass()` issues one
`drawIndirect` whose `vertexCount` is the sum of all submesh index counts.
The existing occlusion cull (`occlusion_cull.slang`: bounding-sphere early-out,
screen-AABB, Hi-Z mip test, OBB corner loop, per-texel refinement) only reduces
the **main pass**; it runs after Hi-Z generation and cannot influence the
prepass that produced the Hi-Z.

The engine's own profiling concern: the prepass is **vertex-transform bound**.
`depth_indir.slang` maps `SV_VertexID → IndirEntry → random StructuredBuffer
fetch (176 B `VertEntry`) → one 4×4 MVP mul` with an empty fragment shader.
The depth *writes* are nearly free (empty FS, early-Z, depth compression);
the vertex fetch + transform of every index is the cost.

**Goal:** reduce objects submitted to the depth prepass using only
current-frame information, accepting a serial but small added GPU chain,
with no last-frame data anywhere in the engine.

---

## 2. Cost model of the current pipeline

### 2.1 Why the prepass is vertex-bound, not depth-bound

- Depth writes: empty fragment shader, early-Z, hierarchical depth compression.
  Nearly free on all modern architectures.
- Vertex cost per index: one 176-byte `StructuredBuffer` fetch (bindless,
  poorly localized — threads within a wave fetch *different* blocks) plus a
  4×4 matrix mul (~64 FMAs). Because the indirection buffer maps
  `vertexId → (buffer, vertex)` directly, **there is no post-transform vertex
  reuse at all**: every index of every triangle is a separate shader
  invocation. A closed mesh transforms each vertex ~6× (once per incident
  triangle) compared to a hardware indexed draw with a post-transform cache.

So the prepass cost is approximately:

```
P ≈ totalIndexCount / V,    V ≈ 2–6 G vertices/s on mid-range modern GPUs
```

for this class of (fetch-heavy, arithmetic-light) vertex shaders. Rules of
thumb:

| Prepass index count | Approx. GPU time (mid-range, 2–6 Gv/s) |
|---|---|
| 1 M   | 0.17–0.5 ms |
| 5 M   | 0.8–2.5 ms |
| 10 M  | 1.7–5 ms   |
| 50 M  | 8–25 ms    |

(For calibration, AMD's GDC 2016 "Optimizing the graphics pipeline with
compute" work targeted a ~40–50 M vertex/frame scene on a Fury X and moved
total vertex processing from ~3 ms down to ~1.3 ms *after* moving culling into
compute — i.e., single-digit-millisecond prepasses are the normal regime for
heavy scenes, and vertex processing is exactly the stage culling targets.)

### 2.2 The asymmetry that motivates all of this

Every object the prepass does *not* draw saves:

1. its vertex fetches + MVP muls (the dominant cost),
2. its triangle setup and rasterization in the prepass,
3. its slot in the compaction/collect passes downstream,
4. nothing in the main pass (main already reads the compacted buffer — but
   the same cullEntries zeroing *does* propagate there for free, since
   `collect_count_compact` and the post-prepass occlusion pass both early-out
   on `indexCount == 0`).

Compute-side culling work, by contrast, is measured in *submeshes* (10⁴–10⁵
threads), not *vertices* (10⁶–10⁷). That is a 2–3 order-of-magnitude
asymmetry in favor of doing culling math on bounds instead of transforms on
vertices. This is the entire economic argument for the design below.

---

## 3. Theory: the occluder/occludee asymmetry

Any bounding-volume-based visibility test is governed by two opposite
conservativeness requirements:

| Role | Requirement | Why |
|---|---|---|
| **Occludee** (the thing being tested) | bound must **over-approximate** the object (contain it) | if even the bound is hidden, the object certainly is; a loose bound can only cause *missed* culls |
| **Occluder** (the thing hiding) | bound must **under-approximate** the object (be strictly inside it) | a loose occluder covers screen area the real mesh does not, causing **false culls** — objects visible through the mesh/box gap disappear |

Consequences:

- A fitted OBB is a **valid occludee** bound and an **invalid occluder**.
  This is precisely why "OBB occludes OBB" fails when both boxes are fitted
  bounds: it is not a precision problem, it is a direction-of-approximation
  problem.
- Shrinking an OBB toward its center does *not* fix this in general: a shrunk
  box is still not guaranteed to be inside the mesh (concavities, thin
  shells). Only authoring guarantees ("this box is inside this wall") or
  actual mesh geometry give valid occluders.
- Hi-Z from *real* rendered geometry is automatically a valid occluder set:
  rasterized depth is a subset of true depth, so any object culled against it
  is truly hidden. This property is what makes the occluder-prepass design
  **conservative by construction**.

### 3.1 The exact OBB-occludes-OBB test (documented, then rejected)

For completeness, the pairwise test: project the occluder's 8 corners, take
the convex hull as a 2D silhouette polygon; the target is fully hidden iff all
8 of its corners project inside that silhouette **and** behind the occluder
(octant/wedge test in screen space). ~O(1) per pair, no z-buffer.

Rejected because:

1. **Union of occluders is the hard part.** "Half behind box A, half behind
   box B" is a 2D *coverage* problem: polygon clipping of the target's
   silhouette against each occluder's silhouette, with area accumulation.
   That is software rasterization with worse numerical behavior and more
   code. The z-buffer computes the same union for free.
2. It needs *tight* occluders, which fitted OBBs are not (see §3).
3. Pair tests scale O(n²); even 10 k × 10 k pairs at ~100 flops is ~10¹⁰
   flops/frame — completely off the table, whereas the z-buffer approach
   amortizes all occluders into one image.

**Rule of thumb:** whenever the question is "is this object hidden by the
*union* of many occluders," the answer is a z-buffer, not analytic geometry.
The z-buffer is the union-machine.

---

## 4. Survey of existing techniques (with real measured data)

### 4.1 Hierarchical-Z (Hi-Z) occlusion culling — the baseline

Classic formulation (Green/Diaz 2007 "March of the Froblins" SIGGRAPH course;
Rákos 2010):

1. Render occluders to a depth buffer.
2. Downsample with **max reduction** (farthest depth per texel) into a mip
   chain — the Hi-Z map.
3. Test each object's screen-space AABB: pick the mip whose texel footprint
   ≈ the AABB (LOD = ceil(log2(max(widthPx, heightPx) / 2))), fetch 2×2
   texels, object is occluded iff its near depth is beyond the *max* of
   those texels.

Measured: Rákos reports the full Hi-Z construction at 1024×768 takes
**< 0.2 ms on a Radeon HD5770 (2010)** — and that construction cost scales
with buffer size, not scene size, which is why low-res occluder buffers are
so cheap. The same article explains why 4 texel fetches at mip *N* beat 1
fetch at mip *N+1*: better footprint fit, simpler LOD math, no effectiveness
collapse for centered objects.

Notes relevant to this engine: our `hiz_gen.slang` already implements max
reduction; our non-reversed [0,1] depth with farthest-depth-in-footprint
matches the "store maximum depth" policy exactly.

### 4.2 NVIDIA nvpro batched occlusion culling sample (Kubisch et al.)

`nvpro-samples/gl_occlusion_culling` — shader-based batched culling of all
scene bboxes at once, 17 576 objects, Quadro K5000 (2012-class), timings in µs:

| Technique | GPU time |
|---|---|
| Frustum cull (compute) | 31–51 µs |
| Hi-Z occlusion cull (depth pass + Hi-Z + test) | (last-frame results mode) |
| Raster-based cull (boxes rasterized invisibly against depth) | 266–286 µs for 17.5 k boxes |

Key measured findings from that sample:

- **Raster-tested boxes cull better than Hi-Z** (12 % vs 34 % objects kept in
  their scene) because the box's actual orientation/dimensions are tested
  per-pixel rather than via AABB approximation — but cost ~5–9× more than
  the compute test.
- "Current frame" culling (build depth from frustum-culled draw, test,
  redraw) *can lose* to no culling when the extra depth pass outweighs the
  savings on small scenes — the break-even analysis in §7 formalizes this.
- Temporal-coherence variants exist (draw last frame's visible first, then
  test only new-visible ones) but are excluded here by the no-temporal
  constraint.
- GPU-side result consumption (indirect draw, no readback) removed ~4.4 ms of
  CPU-side frame time vs CPU readback.

### 4.3 Hand-authored occluder Hi-Z (Darnell 2010; Garpenhall 2025)

Nick Darnell's writeup (GDC "Rendering with Conviction"-derived, Radeon
5450): occluder render (512×256) + mip chain + compute test of bounds =
**0.74 ms total**, with "little difference between 900 bounds and 10,000
bounds" — fixed overhead dominates; per-object marginal cost is negligible
even on 2010 hardware. Recommends artist-authored occluder boxes/planes.

Tobias Garpenhall (The Game Assembly, 2025): 1024×1024 occluder depth,
hand-authored `[OC_]`-tagged cube proxies batched and instanced, whole chain
= **50–200 µs (0.9 % of frame)**; culled ~3500 → ~800 drawn meshes (f ≈ 0.77)
in an apartment scene. Adds pixel-size culling for free in the same shader.

Frostbite uses ~256×114 occlusion buffers; Drobot's (SIGGRAPH 2015,
"Low Latency, Low Complexity Occlusion Culling in Anvil") software-rasterizes
occluders in compute into a small depth buffer (~0.1–0.3 ms) to get
current-frame occlusion without a second draw pass — the fully-compute
variant of step 3 below.

### 4.4 Masked software occlusion culling (Intel)

Anagnostiou & Shiue: 8×8 mask tiles with hierarchical min-depth, SIMD CPU
rasterizer at tens of millions of triangles/s per core. Decouples coverage
from depth so union-of-occluders tests are exact per tile. Not needed here:
a hardware rasterizer at low resolution is strictly cheaper than emulating
this in compute, and we have one.

### 4.5 Hybrid compute+raster culling (Computers & Graphics, 2023)

"A GPU-friendly hybrid occlusion culling algorithm for large scenes":
iterative hierarchical-Z culling in compute for the coarse tier, rasterization
for the fine tier — same two-tier shape as §5.

### 4.6 Hardware occlusion queries — the rejected baseline

Per-object occlusion queries (or their batched `GL_ARB_occlusion_query`
descendants) require either CPU sync or conditional rendering, one query per
object per frame, and pipeline bubbles. The nvpro sample exists precisely to
replace them; this engine's cullEntries/compaction design is already the
GPU-driven replacement, so queries are out.

---

## 5. Proposed design: occluder prepass before the depth prepass

```mermaid
flowchart LR
    E[expand\nMVP + cullEntries\nall submeshes] --> S[select\nscreen-area threshold\natomic-append, capped]
    S --> OD[occluder prepass\nlow-res depth\ne.g. 512x288]
    OD --> HZ[hiz_gen\nsmall mip chain]
    HZ --> PC[pre-cull compute\nOBB vs Hi-Z\ntwo-pass compact]
    PC --> DP[depth prepass\nsurvivors only\nfiltered indirection]
    DP --> HZ2[hiz_gen full res] --> OCC[occlusion pass\nexisting] --> C[collect] --> M[main pass]
```

### 5.1 Pass-by-pass design

**A. Selection (compute, one dispatch, N = submesh count).**
Each thread transforms its OBB's 8 corners with the MVP already computed by
expand, produces the screen AABB, computes area, and if

```
area ≥ kMinAreaPx²  &&  listCount < kMaxOccluders
```

appends its submesh id via `InterlockedAdd` to a small candidate list
(capped at e.g. 256–1024 entries). No sort, no scan, no ranking: *any* set of
large occluders is a valid occluder set — selection can only miss culls,
never cause false ones (§2). Optionally also require
`indexRange ≤ kMaxOccluderTris` so a giant candidate cannot blow the occluder
prepass budget.

Threshold tuning: a single fixed threshold such as "OBB covers ≥ 0.25–0.5 %
of the screen" self-adapts (few occluders facing the sky, more inside
rooms). If budget overflow becomes routine, upgrade to a 256-bin histogram of
`log2(area)` + one reduction pass to pick the cutoff bin — two cheap passes,
still no sort. (Full sorts — bitonic, or NVIDIA oneSweep radix at ~0.1–0.2 ms
per 1 M keys — are overkill for occluder selection.)

**B. Occluder prepass.** Hardware raster of the *actual meshes* of selected
candidates into a low-res depth image (e.g. 512×288, clear to 1.0 = far).
Reuse the depth-prepass vertex path (it is already indirection-driven), with
a second filtered indirection buffer written by a small compaction over the
candidate list. Crucially this draws **real geometry**, so the depth is a
valid under-approximation for free; no OBB-silhouette or proxy generation is
needed (and fitted-OBB silhouettes would be *wrong* — §3).

Alternative for later: Drobot-style coarse rasterization of the candidates in
compute into a ~256×144 depth buffer, avoiding the graphics-pipeline round
trip. More code, marginal gain at this scale; the hardware path is simpler
and the rasterizer is otherwise idle at this point in the frame.

**C. Small Hi-Z chain.** Reuse `hiz_gen.slang` as-is on the 512×288 image
(~9 mips, a handful of tiny dispatches). Measured cost class: <0.2 ms for a
*full-screen* chain on 2010 hardware (§4.1); this is a quarter of that.

**D. Pre-cull compute + compaction.** Per-submesh thread: frustum reject
(OBB all-outside any plane or AABB off-screen — code already exists in
`occlusion_cull.slang` lines 116–146/157–214), then the same near-pole /
`SampleLevel` test against the small Hi-Z, then the two-pass compaction
(clone of `collect_count_compact.slang` minus the per-technique
`sharedOffsets` machinery — single-technique variant, ~50 lines). Output:

- `cullEntries[i].indexCount = 0` for culled submeshes (so the existing
  post-prepass occlusion pass and collect pass skip them for free), and
- a filtered indirection buffer + `DrawIndirectCommand` whose `vertexCount` =
  sum of surviving index counts, consumed by `DepthPrepass()` instead of the
  expand-written total.

**E. Full depth prepass.** Unchanged except the bound buffer/count. Optional
refinement: keep the occluder depth in the buffer (`load_op = LOAD` for the
full prepass) so survivors' depth pass only adds new content.

### 5.2 What this buys structurally

- **Current-frame occlusion, zero temporal state.** Every cull decision is
  made against depth rasterized *this frame* from real geometry. No
  camera-cut handling, no first-frame special case, no pop-in risk beyond
  normal conservative false-negatives (objects kept alive).
- **Reuse, as identified in discussion:** the post-prepass occlusion pass
  only ever sees survivors (culled entries are already 0), the compacted
  indirection pattern is shared, and the small Hi-Z logic is the existing
  `hiz_gen`.

---

## 6. Step-by-step cost estimates

Assumptions: 100 k submeshes, 1080p-class frame, modern mid-range GPU
(RDNA 3 / Ada class). Dispatch overhead ~5–15 µs each; barrier ~ few µs.

| Step | Work | Cost estimate |
|---|---|---|
| Selection compute | 8 muls + AABB + area + 1 atomic per submesh; ~200 flops × 100 k = 20 Mflop | 5–15 µs |
| Occluder prepass | 256–1024 real meshes, vertex-bound; at ≤1 M occluder verts | 30–80 µs |
| Small Hi-Z chain | ~9 mips of ≤512×288 reductions | 10–20 µs (Rákos: <0.2 ms full-res on 2010 GPU) |
| Pre-cull test | OBB → AABB + 1 SampleLevel (+ optional refine) per submesh | 20–50 µs |
| Two-pass compact | clone of collect count+compact, 100 k entries | 20–40 µs |
| Extra dispatch/barrier overhead | ~6 extra small submissions on the critical path | 50–100 µs |
| **Total added** | | **≈ 0.15–0.3 ms**, mostly fixed |

Compare with the *removed* work when fraction *f* of the prepass is culled:
`f × P` where `P ≈ totalIndexCount / (2–6 G verts/s)`. The chain is
serial (select → draw → hi-z → test → compact → prepass) and sits on the
critical path after expand; there is no realistic async-compute overlap — but
each added step is an order of magnitude smaller than the passes it sits
between, and the nvpro numbers (frustum cull of 17.5 k boxes in 31–51 µs on
2012 silicon) show the per-object marginal cost is tiny.

---

## 7. Break-even analysis

Net win condition: `f × P > A`, with `A ≈ 0.2 ms` added cost, `P` prepass
time, `f` occlusion-culled fraction (after frustum).

| Prepass size | P (at ~4 Gv/s) | Break-even f |
|---|---|---|
| 1 M verts   | ~0.25 ms | ≈ 100 % — **don't bother** |
| 2 M verts   | ~0.5 ms  | ≈ 30–50 % |
| 10 M verts  | ~2.5 ms  | ≈ 6–10 % |
| 50 M verts  | ~12 ms   | ≈ 1–2 % |

Reference points for achievable `f`:

- Garpenhall (apartment interior): 3500 → 800 drawn meshes, **f ≈ 77 %**.
- nvpro raster culling: 12 % of objects visible in a dense box-grid scene.
- General experience: interiors/corridors/cities routinely reach f > 50 %;
  open vistas go to ~0 %.

Corollary: the design's *cost* is scene-independent (~0.2 ms fixed) while its
*payoff* is scene-dependent — the exact inverse of the un-culled prepass,
whose cost is scene-dependent. The failure case is a scene of small
non-occluding clutter with nothing big and near; then the chain spends 0.2 ms
to learn that nothing is hidden.

Decision guidance: if the prepass is below ~1 M vertices, don't build this —
eat the transforms. Above ~5 M vertices in scenes with any architectural
occlusion, it pays for itself by roughly an order of magnitude over break-even.

---

## 8. Correctness

Claim: the pipeline never falsely culls. Proof sketch:

1. Occluder prepass rasterizes actual triangle geometry with a normal
   (LEQUAL) depth test; the resulting depth value at any pixel is ≥ true
   scene depth at that pixel (occluder depth is real geometry depth, and
   nothing farther can be recorded due to the depth test).
2. Hi-Z mip stores the *farthest* occluder depth per footprint.
3. A submesh is culled only if every Hi-Z texel in its conservative screen
   AABB holds depth strictly in front of the object's conservative near
   bound (near pole of its bounding sphere — existing convention in
   `occlusion_cull.slang`, lines 168–179, which avoids the min-over-corners
   false-cull hazard).
4. Then every real surface point of the object projects into the AABB at a
   depth beyond the recorded occluder depth, which is real depth → the
   object is fully covered by real, nearer geometry → invisible.

No assumption about frame-to-frame coherence is used anywhere, so camera
cuts, teleports, and the first frame are all correct by construction.
(Conservative `near pole` depth, not corner-min, must be preserved in the
pre-cull shader — the corner-min variant is documented in the shader as a
false-cull hazard.)

Remaining correctness caveat: the *effectiveness* is bounded by occluder
selection (missed culls only) and by the small Hi-Z resolution; at 512×288 an
object smaller than ~one texel cannot be proven occluded. That is by design —
sub-pixel objects are candidates for size culling, not occlusion culling.

---

## 9. Failure modes

| Symptom | Cause | Fix |
|---|---|---|
| Objects pop in at screen edges | OBB screen-AABB clamping eating off-screen extents | keep the unclamped one-sided off-screen test (already in occlusion_cull.slang) |
| Objects pop behind large close walls | Occluder set misses a dominant occluder (threshold too high / budget too low) | lower threshold, raise cap; histogram if needed |
| Everything culled behind camera | `clipPos.w <= 0` mishandled | existing near-pole guard (lines 119–123) must be ported to pre-cull |
| Small Hi-Z says visible but full depth disagrees | expected: low-res occluder buffer is coarser than full prepass | not a bug; post-prepass pass catches the rest |
| y-flip / NDC sign bugs | depth image row 0 = NDC +y | existing uvA/uvB sort pattern (lines 220–223) must be ported |

Debug tooling worth building with the feature: visualize the small Hi-Z mip
chain and a per-submesh "culled by" enum (none / frustum / occlusion) — both
are single buffers readable via the existing debug-readback paths.

---

## 10. Implementation mapping (this engine)

| Engine piece | Reuse / change |
|---|---|
| Selection + area compute | new small compute pass; MVP data already produced by `expand.slang` (could even fuse area computation into expand itself, removing one pass) |
| Occluder prepass | clone of `DepthPrepass` with low-res viewport + filtered indirection set; new render-graph node between expand and depth-prepass in `Renderer.cpp` |
| Small Hi-Z | `DispatchHiZGen` parameterized for the small image, or a second hiz pipeline instance |
| Pre-cull test | `occlusion_cull.slang` minus the refine loop, plus threshold/append in expand |
| Compaction | `collect_count_compact.slang` with `techniqueCount == 1` semantics; count pass reuses `indexCount == 0` convention |
| Buffers | one `BlockArray` for the candidate list + filtered indirection (capacity = total, same pattern as `submesh_cull`) |
| Render graph | new nodes between `expand` and `depth-prepass`; reads `hiz-image` (small), writes `scene-buffers` + `draw-indirect`; existing `AddDependency` chain extended |

Phasing:

1. **Frustum-only pre-cull** (no occluder prepass): biggest win per line of
   code, no new resources, works frame 1.
2. **Occluder prepass chain** as above with fixed threshold and no sort.
3. **Refinements** (only if profiling demands): histogram-based occluder
   budget, refine loop in pre-cull, artist occluder proxies, compute-side
   coarse rasterization.

Explicitly deferred: temporal/last-frame Hi-Z (rejected by requirement),
pairwise OBB-occlusion analytic tests (§3.1), full software occlusion
buffers (Intel MOC) — a hardware z-buffer at 512×288 dominates that design
for this engine.

---

## 11. References

- D. Rákos, "Hierarchical-Z map based occlusion culling", RasterGrid, 2010 —
  Hi-Z construction details; <0.2 ms construction on HD5770.
  https://rastergrid.com/blog/2010/10/hierarchical-z-map-based-occlusion-culling/
- N. Darnell, "Hierarchical Z-Buffer Occlusion Culling", 2010 — author
  occluders, 512×256 buffer, 0.74 ms total on Radeon 5450, 900–10 000 bounds.
  https://www.nickdarnell.com/hierarchical-z-buffer-occlusion-culling/
- T. Garpenhall, "Occlusion Culling" (The Game Assembly, 2025) — production
  numbers: 50–200 µs, 0.9 % frame, 3500→800 meshes, `[OC_]` proxies.
  https://www.tobiasgarpenhall.com/occlusion-culling
- nvpro-samples `gl_occlusion_culling` (Kubisch) — batched Hi-Z/raster cull
  timings (frustum 31–51 µs; raster cull 266–286 µs; 17 576 objects).
  https://github.com/nvpro-samples/gl_occlusion_culling
- Intel GameTechDev, "Masked Software Occlusion Culling" (Anagnostiou &
  Shiue) — software occlusion buffer reference.
  https://github.com/GameTechDev/MaskedOcclusionCulling
- "A GPU-friendly hybrid occlusion culling algorithm for large scenes",
  Computers & Graphics, 2023 — compute coarse cull + raster fine cull.
  https://www.sciencedirect.com/science/article/pii/S014193822300166X
- AMD "March of the Froblins" SIGGRAPH 2008 course (Hi-Z + sbt occlusion
  origins). https://developer.amd.com/wordpress/media/2013/07/Chapter03-SBOT-March_of_The_Froblins.pdf
- Drobot, "Low Latency, Low Complexity Occlusion Culling in Anvil",
  SIGGRAPH 2015 (paywalled; summary: compute coarse raster of occluders into
  small depth + object tests, ~0.1–0.3 ms class).
- GPU Pro 7, "Software-based occlusion culling" chapters (CPU SIMD raster
  variants; useful only if CPU-side culling is ever added).
