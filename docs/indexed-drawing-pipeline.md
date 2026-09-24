# Indexed drawing: decisions and performance data

This document records the design decisions and performance data behind hardware
indexed draws: why occurrence-based indirection is replaced, and what the
replacement costs and saves.

## 1. The problem with occurrence indirection

An occurrence-based indirection writes one entry per index occurrence. The vertex
shader reads `vertexId → entry → vertex`, so every index is a separate
vertex-shader invocation and there is no post-transform vertex reuse. A closed
mesh transforms each vertex about six times, once per incident triangle.

Hardware indexed draws restore the post-transform vertex cache. Vertex-shader
invocations drop by roughly `r / ACMR`, where `r` is the index-to-vertex ratio and
`ACMR` is the average cache miss ratio:

- `r` is typically about 1.5–2.5.
- `ACMR` is about 1.0–1.3 for unoptimized exports and about 0.6 for
  cache-optimized meshes.
- The observed vertex-shader reduction is about 1.5–2× for unoptimized assets and
  about 2.5–3.3× for cache-optimized assets.

Because indirection maps `vertexId` directly to a vertex, the fetch is a random
`StructuredBuffer` read. Indexed draws keep that fetch path; the win comes only
from the cache, not from a change to the fetch.

## 2. Decisions

### 2.1 Use hardware indexed draws

Compaction output is a list of 4 B absolute slot indices. Each pass binds that
list as an index buffer and issues an indexed indirect draw.

### 2.2 Two draw modes, both pixel-identical

Two modes share one vertex-fetch path and differ only in the compaction output:

| | Monolithic | Multi-indirect-draw (MID) |
|---|---|---|
| Compaction output | 4 B absolute slot indices, packed per pass/technique | 20 B `DrawIndexedIndirectCommand` per alive submesh + a 4 B count |
| Depth/occluder prepass | one `drawIndexedIndirect` with a GPU-written command | `drawIndexedIndirectCount` |
| Main pass | one `drawIndexedIndirect` per technique | one `drawIndexedIndirectCount` per technique |
| Commands per technique | 1 | one per alive submesh |
| Vulkan feature | none (`drawIndexedIndirect` is core 1.0) | `drawIndirectCount` (Vulkan 1.2 core / `VK_KHR_draw_indirect_count`) |

Both modes produce the same survivors and the same vertex-cache behavior. They are
an A/B pair and a device-capability fallback, not a per-frame switch.

### 2.3 `drawIndirectCount` is optional

Monolithic mode needs no feature beyond core 1.0. MID mode needs
`drawIndirectCount`, which is optional at device creation. When it is not
available, fall back to monolithic.

### 2.4 One specialization constant selects the emission variant

Both modes compile from the same shader source. A specialization constant selects
the emission variant in the compaction shaders, so only the active mode's pipeline
is created. This keeps one source of truth and keeps shader hot reload working.

### 2.5 Tight per-submesh vertex window

An entry per vertex of a multi-submesh mesh costs O(K·V). Instead, each submesh
carries a tight window over the vertex range it uses:

```
vertexWindowBase = min over the submesh's indices
vertexSpan       = max − min + 1
```

The window is computed once at upload from the CPU index data. The index
convention is corrected at the same time:

```
draw_indices = entryBase + (rawIndex − vertexWindowBase)
```

The entry slot `j ∈ [0, vertexSpan)` maps to absolute vertex
`baseVertex + vertexWindowBase + j`. A 24-bit vertex pack bounds
`baseVertex + vertexWindowBase + vertexSpan`; enforce this at upload and fail
loudly, never mask silently.

### 2.6 Streamed meshes must keep topology stable

The vertex window is computed at registration. A topology change (index count or
per-submesh ranges) requires re-registration.

### 2.7 Per-technique command regions use a CPU prefix sum

`vkCmdDrawIndexedIndirectCount`'s `offset` argument is a CPU command-recording
value; it cannot be GPU-written in the same frame. The main pass therefore uses a
CPU-computed prefix sum, taken during gather:

- `submeshCount[t]` = number of submeshes whose material maps to technique `t`.
- `techniqueRegionBases[t] = prefix_sum(submeshCount[0..t-1])`.
- The command buffer has `totalSubmeshes` slots.

The `maxDrawCount` VUID holds by construction:
`20 × (submeshCount[t] − 1) + techniqueRegionBases[t] × 20 + 20 =
20 × techniqueRegionBases[t+1] ≤ 20 × S`. No padding, no readback.

### 2.8 Per-mode buffers and usage flags

Buffers are sized and usage-flagged per mode. No aliasing and no dual-purpose
buffers, so the validation layer catches accidental cross-mode use. A mode change
re-creates the mode-dependent frame buffers with the target mode's flags and
sizes, and rebuilds the specialization-constant pipelines. Descriptor-set layouts,
pools, and technique pipelines are mode-independent and are reused.

### 2.9 Capacity follows the scene

Frame buffers size from the scene's current totals. The totals are known only
after the gather pass, so buffers are allocated at an initial capacity and grow
geometrically from the gather. Growth device-idles and re-creates the
capacity-dependent buffers. Descriptors are written per frame, so growth and mode
changes need no descriptor surgery.

## 3. Cost data

Symbols: `N` = total index count, `S` = submeshes, `r` = index/vertex ratio
(1.5–2.5), `ACMR` = average cache miss ratio.

| Metric | Occurrence indirection | Monolithic | MID |
|---|---|---|---|
| Indirection VRAM/frame | 4 × 8 B × N = **32 B/index** | 8 B/r (entries) + 4 B (draw indices) + 3 × 4 B (compact copies) ≈ **16–20 B/index** | 8 B/r + 4 B + ~0 (commands) ≈ **12 B/index** |
| expand traffic/index | 12 B (4r read + 8 write) | ~12 B (4r + 4w + 8/r) — neutral | same |
| Compaction copies/pass | 16 B/index (read + write of 8 B) | **8 B/index** | ~0 (20 B/submesh) |
| Copy-traffic saving at 2 M visible indices | — | ~48 MB/frame | ~96 MB/frame (≈0.1–0.25 ms) |
| Vertex-shader invocations | 1 per occurrence | **÷ r/ACMR ≈ 1.5–3.3×** | same |
| Draw calls | 1 prepass + 1 occluder + 1/technique | unchanged | unchanged |
| Vulkan features | — | none beyond core 1.0 | `drawIndirectCount` (optional) |

Asset-quality dependency: the vertex-shader gain is `r / ACMR` per submesh. The
post-transform cache (historically 16–32 entries) converts occurrences to unique
vertices well only when the index order has locality. Unoptimized exports
(`ACMR` ≈ 1.0–1.3) land at the low end (about 1.5–2×); cache-optimized meshes land
at the high end (about 2.5–3.3×).

## 4. Other decisions

- Compaction shaders keep their culling logic; only the emission changes. In
  monolithic mode they copy a 4 B index; in MID mode they emit a 20 B command and
  increment a count.
- Occlusion culling is unchanged in both modes: the zero-index-count convention
  and the occluder flag still gate survivors.
- MID publishes per-technique alive counts from the compact pass with an atomic
  increment (one per alive submesh). This is race-free and removes the need for a
  separate epilogue dispatch.
- Do not have the compactors read asset index buffers and convert on the fly.
  Three consumers would each redo the conversion; write the index list once and
  copy from it.
