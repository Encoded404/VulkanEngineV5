# Indexed Drawing Pipeline: Implementation Plan (rev 2)

**Scope:** replace the occurrence-based indirection (`IndirEntry` per index occurrence, single non-indexed `drawIndirect` per pass) with hardware indexed draws, so the GPU's post-transform vertex cache eliminates redundant vertex-shader invocations in the occluder prepass, depth prepass, and main pass.

This revision supersedes the original plan and corrects two things:

1. **It was written against the pre-phase-1 pipeline** (a "depth_filter" survivor pass that no longer exists). The pipeline now runs the full §5.3 chain from `docs/pre-prepass-occlusion-culling.md`:
   `expand → occluder-select → occluder-prepass (clear) → hiz-gen-pre → pre-cull → depth-prepass (load) → hiz-gen (full) → occlusion → collect → main`.
   There are **three** consumers of occurrence indirection today (depth survivors, occluders, main pass), not two, and the survivor compaction lives in `pre_cull.slang` — there is no `depth_filter.slang`.
2. **The compaction strategy changed.** Instead of jumping straight to per-submesh indirect commands (multi-indirect-draw, "MID"), the default implementation keeps today's monolithic-draw architecture and compacts **4 B absolute slot indices** instead of 8 B `IndirEntry`s. The per-submesh-command design (MID) is kept as a second draw mode behind an initialization-time setting. Both modes share the entire substrate (expand output, `vertex_entries`, `draw_indices`, cull entries, vertex fetch); only the compaction emission and the draw calls differ.

**Non-goals / unchanged:** occlusion culling decision logic, Hi-Z, mesh upload paths, bindless vertex fetch, technique system, render-graph topology. Vertex-cache optimization of asset index buffers (e.g. meshoptimizer) remains a non-goal — see §8 for the impact.

---

## 0. Draw modes

| | **Monolithic** (default) | **MID** (multi-indirect-draw) |
|---|---|---|
| Compaction output | 4 B absolute slot indices, packed per pass/technique | 20 B `DrawIndexedIndirectCommand` per alive submesh + 4 B counts |
| Depth/occluder prepass draw | one `drawIndexedIndirect` with a GPU-written command | `drawIndexedIndirectCount` over a command buffer |
| Main pass draw | one `drawIndexedIndirect` per technique (`indexCount = Σ` alive submesh indices, `firstIndex = technique offset`) | one `drawIndexedIndirectCount` per technique over per-submesh commands |
| Commands per technique | 1 (unchanged from today) | N (one per alive submesh) |
| Vulkan feature needed | none (`drawIndexedIndirect` is core 1.0) | `drawIndirectCount` (Vulkan 1.2 core / `VK_KHR_draw_indirect_count`) |
| Compaction copy traffic | 8 B/index (r+w of 4 B indices) | ~0 (20 B/submesh) |
| Selection | default | advanced-settings toggle |

Both modes are **pixel-identical** (same survivors, same fetch path, same vertex cache behavior) and are intended as an A/B pair and a device-capability fallback, not a per-frame switch:

- **Mode is fixed at renderer initialization.** `SceneRenderer::Initialize` takes a `DrawMode` parameter threaded from engine config (advanced settings).
- **Changing the mode re-creates the frame ring**: device idle → tear down mode-dependent frame resources → re-run the per-frame resource-creation block in `SceneRenderer.cpp` with the re-queried `tic` (total index count) and submesh capacity → rebind the static descriptor sets (the block at `SceneRenderer.cpp` ~592–766). Mesh-side data (`StaticEntry`, asset index/vertex buffers, `vertex_entries` inputs) is mode-independent and untouched. **Gotcha:** the reinit must re-query current `tic`/submesh capacity, not reuse stale values from first init, or the buffers silently under-size.
- **Only the active mode's pipelines are created.** The two variants come from the same shader sources via one Slang specialization constant (`kCompactionMode`); the inactive variant is never compiled into a pipeline. ShaderWatcher hot-reload keeps working (both variants derive from one source file).
- **Buffers are sized and usage-flagged per mode** (§2) — no aliasing, no dual-purpose buffers. The validation layer then catches any accidental cross-mode use (e.g. drawing from a buffer lacking `eIndexBuffer`).

---

## 1. Data model

### Today

`expand` flattens every submesh's index buffer into `indirection_buffer` — one 8 B `IndirEntry {vertexId, submeshId}` **per index occurrence** (`totalIndexCount × 8` B per frame). The occluder prepass draws from `occluder_indirection_buffer` (compacted by `occluder_select`), the depth prepass from `depth_indirection_buffer` (compacted by `pre_cull`), the main pass from `compacted_indirection_buffer` (compacted by collect). **Four** buffers of 8 B × totalIndexCount are allocated per frame (not 3 — `SceneRenderer.cpp` allocates all four, ~472–523). Every occurrence is a separate vertex-shader invocation with zero reuse.

### Shared substrate (both modes)

| Buffer | Content | Size | Written |
|---|---|---|---|
| `vertex_entries` | one `IndirEntry {packedVid, submeshId}` per **vertex in the submesh's vertex span** — same struct as today, indexed by *slot* instead of occurrence | 8 B × Σ vertexSpan | per frame (expand) |
| `draw_indices` | values = absolute slots into `vertex_entries`. Per occurrence: `entryBase + (rawIndex − baseVertex)` | 4 B × totalIndexCount | per frame (expand) |
| cull entries (existing) | **struct unchanged**: `{indirOffset, indexCount, techniqueId, pad}` — `indirOffset` now means *slot base of the submesh's indices in `draw_indices`* (same value expand allocated before). Rename optional, semantic only | 16 B × submeshes | per frame (expand) |
| counters | entry-base + index-base atomic counters for expand; zeroed by the CPU each frame (alongside the existing counter uploads in `PrepareCompute`) | 8 B | per frame |

Key properties (unchanged from rev 1):

- **No dedup anywhere.** The vertex span `[baseVertex, baseVertex + vertexSpan)` is the unique set; expand's flattening was the only duplication and it is removed.
- **Per-vertex submesh info is preserved** — it lives in the entries, fetched by index value. Culling granularity, technique handling, and the VS pull code are structurally unchanged.
- **Occlusion chain untouched**: `occlusion_cull.slang` (zeroes `indexCount`), the `CullEntry.pad` occluder bit, and the technique flag gating are identical in both modes.

New static per-submesh data: **`vertexSpan`** (vertex count of the submesh's vertex range), sourced at upload/gather from the mesh accessors (or `max−min` over the submesh's indices), carried in `StaticEntry` (one extra u32). The 24-bit `baseVertex` packing limit is unchanged and now also bounds `vertexSpan` arithmetic.

### Per-mode buffers (replacing the four 8 B × N indirection buffers)

| Destination | Monolithic | MID |
|---|---|---|
| main-pass output | `main_compact_indices`: 4 B × N, `{eStorageBuffer \| eIndexBuffer}` | `main_commands`: 20 B × S + `MAX_TECHNIQUES` pad, `{eStorageBuffer \| eIndirectBuffer}` |
| depth-prepass survivors | `depth_compact_indices`: 4 B × N, `{eStorageBuffer \| eIndexBuffer}` | `depth_commands` + count: 20 B × S + 4 B, `{eStorageBuffer \| eIndirectBuffer}` |
| occluder-prepass set | `occluder_compact_indices`: 4 B × N, `{eStorageBuffer \| eIndexBuffer}` | `occluder_commands` + 4 B count |
| `draw_indices` | `{eStorageBuffer}` (compaction source only) | `{eStorageBuffer \| eIndexBuffer}` (bound for draws) |
| `technique_draw_commands` | existing 20 B × `MAX_TECHNIQUES` | 20 B × (S + `MAX_TECHNIQUES` pad) |
| `tech_counts_buffer` | unchanged | gains `eIndirectBuffer` |

In monolithic mode the prepass/occluder "count" buffers stay `DrawIndexedIndirectCommand`-sized (20 B now, was 16 B), CPU-preinitialized to `{indexCount = 0, instanceCount = 1, firstIndex = 0, vertexOffset = 0, firstInstance = 0}`; pre-cull / occluder-select accumulate into word 0 exactly as they do today into `prepassCounter`/`occluderCounter`.

Deleted in both modes: `indirection_buffer`, `compacted_indirection_buffer`, `depth_indirection_buffer`, `occluder_indirection_buffer` — **four** buffers of 8 B × totalIndexCount (32 B/index; the original plan's "~3 × 8 B" undercounted).

---

## 2. Per-component changes

### Shaders

A single specialization constant (`kCompactionMode`, 0 = monolithic, 1 = MID) selects the emission variant in the compaction shaders. Both variants compile from the same source; only the active mode's pipelines are created at init.

**`expand.slang`** — the only substantial rewrite, identical in both modes. Rewrite the body after the matrix/`cullEntries` setup:
- Allocate `entryBase` via `InterlockedAdd(counter, vertexSpan)` and `indexBase` via `InterlockedAdd(counter, indexRange)` (new counter words; zeroed per frame).
- Write `vertex_entries[entryBase + j] = {(vertexBufSlot << 24) | (baseVertex + j), id}` for `j < vertexSpan`. No index reads needed for this.
- For `i < indexRange`: read `rawIndex` from `indexBuffers[slot][offset + i]`, write `draw_indices[indexBase + i] = entryBase + (rawIndex − baseVertex)`.
- Write `cullEntries[id] = {indexBase, indexRange, techId, 0}`.
- Delete the per-occurrence `IndirEntry` stamping loop.

Do **not** have the compactors read asset index buffers and convert on the fly: three consumers (collect, pre-cull, occluder-select) would each redo the conversion; writing `draw_indices` once and copying from it wins.

**`depth_indir.slang`** — binding rename only: set 3 now binds `vertex_entries`; fetch is `vertex_entries[vertexId]` (indexed draws give `SV_VertexID = slot + vertexOffset`, `vertexOffset = 0`).

**`main_indir.slang`** — one-line change: `vertex_entries[vertexId]` (drop `+ baseVertex` / `SV_StartVertexLocation`).

**`pre_cull.slang`** (the original plan's "depth_filter" — this is where survivor compaction lives, bindings 6/7/8) and **`occluder_select.slang`** — decision logic (frustum, Hi-Z, OBB tests, occluder bit, flag gating) is **unchanged**; only the survivor/occluder emission changes:
- *Monolithic*: copy `draw_indices[indirOffset + i]` (uint, 4 B) into the destination index buffer instead of `IndirEntry` (8 B). Counter semantics unchanged.
- *MID*: emit one `DrawIndexedIndirectCommand {indexCount, 1, indirOffset, 0, 0}` per survivor/occluder + `InterlockedAdd(count, 1)`.

**`collect_count_compact.slang`** — count pass **unchanged in monolithic mode** (`InterlockedAdd(inter[tech].count, entry.indexCount)` already sums indices); compact pass copies 4 B indices instead of 8 B entries. *MID*: count per submesh, compact emits one 20 B command per alive submesh, offsets are command-slot offsets.

**`collect_write.slang`** — *Monolithic*: `DrawIndexedIndirectCommand(count, 1, entryOffset, 0, 0)` per technique (`vertexCount/firstVertex` → `indexCount/firstIndex`). *MID*: counts/offsets only (commands come from the compact pass).

**`occlusion_cull.slang`** — **no changes** in either mode.

### C++ — `VulkanCapabilities.cppm` / `.cpp` (MID mode only)

- Add `Feature::DrawIndirectCount` to the enum + `kFeatureCatalog` (`{ "drawIndirectCount", Required }`), enable `Vulkan12Features::drawIndirectCount`, wire the requirement check. Monolithic mode needs nothing new (`drawIndexedIndirect` is core 1.0). No `drawIndirectFirstInstance` needed (sid travels in the entries).

### C++ — `SceneRenderer.cpp` / `.cppm`

- `DrawMode` enum member (from engine config), threaded into `Initialize`.
- Buffers per §1 table, allocated per mode (usage flags per mode — this is deliberate: the validation layer catches cross-mode misuse). `vertex_entries` (8 B × maxTotalSpan), `draw_indices` (4 B × maxTotalIndexCount) exist in both.
- Descriptor sets: the indirection sets (set 3 of the depth layout, `indirection_raw_set` of technique layouts) bind `vertex_entries` — same `IndirEntry` shape, layouts unchanged.
- Compaction pipelines: created for the active mode only (specialization constant `kCompactionMode` on `pre_cull`, `occluder_select`, `collect_count_compact`, `collect_write`).
- **Reinit path**: mode change = device idle → destroy frame ring → re-run the resource-creation block (reuse `Shutdown()`/`Initialize()`); re-query `tic` + submesh capacity at reinit.

### C++ — `SceneRendererFrame.cpp`

- **Monolithic**:
  - `DepthPrepass` / `OccluderPrepass`: `bindIndexBuffer(depth/occluder_compact_indices, 0, eUint32)` + one `drawIndexedIndirect` from the GPU-written 20 B command (accumulated by pre-cull / occluder-select).
  - `Render`: `bindIndexBuffer(main_compact_indices)`; per technique one `drawIndexedIndirect(technique_draw_commands, t * 20, 1, 20)` — command = `{count, 1, offset, 0, 0}`.
  - Compaction shaders copy 4 B `uint`s from `draw_indices` — halved copy traffic, all copy loops and culling logic keep their structure.
- **MID**:
  - Prepass: `drawIndexedIndirectCount(commands, 0, submeshCapacity, 20, count_buffer, 0)` — offset 0, satisfies VUID `maxDrawCount-03143` exactly.
  - `Render`: `drawIndexedIndirectCount(technique_draw_commands, tech_offsets[t] * 20, MAX_TECHNIQUES, 20, tech_counts_buffer, t * 4)` — **requires the buffer padded with `MAX_TECHNIQUES` extra command slots** (see §5, VUID `maxDrawCount-03143`: `stride × (maxDrawCount − 1) + offset + 20 ≤ buffer size` must hold at record time for worst-case GPU-computed offsets).
  - `indexCount == 0` commands (culled submeshes, empty techniques) are skipped by spec.
- **Barriers (both modes)**: `draw_indices` and any compact-index destination: `eShaderWrite → eIndexRead`, `eComputeShader → eVertexInput`; command/count buffers: `eShaderWrite → eIndirectCommandRead` / `eDrawIndirect`; entries keep the existing shader-read barrier. Wire via the render-graph pass access flags in `src/engine/render/passes/` (`ExpandPass`, `PreCullPass`, `OccluderPrePass`, `DepthPrePass`, `CollectPass`) — mode-dependent edges (`eIndexRead` vs `eIndirectCommandRead`) are chosen at graph build (init), not per frame.

### C++ — `MeshGatherSystem.cpp` / mesh data

- Add `vertexSpan` to `StaticEntry` (Slang struct in `expand.slang` + C++ mirror + `static_assert(sizeof(StaticEntry) == 24)`) and to `SubMesh`, populated from the accessor vertex count at upload; the streamed path (`UpdateStreamed`) populates it too. Update the `compact_static` block config (`make_block_config(20, …)` → 24) in `SceneRenderer.cpp`. No changes to `MeshUploadManager` — source index/vertex buffers are consumed as-is (the bindless index set remains as expand's *source* data).

### Deleted (both modes)

`indirection_buffer`, `compacted_indirection_buffer`, `depth_indirection_buffer`, `occluder_indirection_buffer` (8 B × N each), the per-occurrence stamping loop. In monolithic mode the 8 B entries are replaced by 4 B compact copies; in MID mode by ~20 B/submesh command buffers.

---

## 3. File change list

| File | Change |
|---|---|
| `src/engine/shaders/expand.slang` | rewrite: entries + `draw_indices` replay + counter atomics; delete entry stamping |
| `src/engine/shaders/main_indir.slang` | one line: drop `baseVertex`, fetch `vertex_entries[vertexId]` |
| `src/engine/shaders/depth_indir.slang` | binding rename to `vertex_entries` |
| `src/engine/shaders/pre_cull.slang` | survivor emission: index copy (mono) / command emission (MID), `kCompactionMode` |
| `src/engine/shaders/occluder_select.slang` | same split for the occluder leg |
| `src/engine/shaders/collect_count_compact.slang` | 4 B index copy / command emission, spec constant |
| `src/engine/shaders/collect_write.slang` | indexed command fields (mono) / counts+offsets only (MID) |
| `src/engine/shaders/occlusion_cull.slang` | unchanged |
| `src/engine/render/SceneRenderer.cppm` | `DrawMode`, frame-struct buffer members, feature check, config plumbing |
| `src/engine/render/SceneRenderer.cpp` | per-mode buffer allocation + usage flags, descriptor bindings, pipeline creation for active variant, reinit path |
| `src/engine/render/SceneRendererFrame.cpp` | `DepthPrepass`/`OccluderPrepass`/`Render` draw calls, `DispatchCollect` counters |
| `src/engine/render/MeshGatherSystem.cpp` | `StaticEntry` + `vertexSpan` (20→24 B), mirror + `static_assert`, streamed path |
| `src/engine/render/MeshUploadManager.*` / `SubMesh` | expose `vertexSpan` from accessors |
| `src/backend/vulkan/VulkanCapabilities.cppm` / `.cpp` | `drawIndirectCount` feature catalog entry (MID only) |
| `src/engine/render/passes/*.cpp` | render-graph access flags per mode (`eIndexRead` vs `eIndirectCommandRead`, `eVertexInput` vs `eDrawIndirect`) |

---

## 4. Implementation order

1. **Prereqs**: add `vertexSpan` through `SubMesh` → `StaticEntry` (20→24 B, block config, streamed path); add the `DrawMode` setting.
2. **Buffers & descriptors** (`SceneRenderer.cpp/.cppm`): `vertex_entries`, `draw_indices`, per-mode destination buffers with per-mode usage flags; rebind indirection sets to `vertex_entries`; counter buffers.
3. **`expand.slang`** rewrite (entries + index replay + counters).
4. **Vertex shaders**: `main_indir` baseVertex removal; `depth_indir` binding rename.
5. **Compaction shaders + collect_write**: monolithic variants first (4 B copies, indexed command fields) with the `kCompactionMode` specialization constant in place.
6. **Draw calls + barriers** (monolithic): `DepthPrepass`/`OccluderPrepass`/`Render` switch to `bindIndexBuffer` + `drawIndexedIndirect` against per-destination compact index buffers; render-graph flags; counter zeroing in `PrepareCompute`.
7. **Validation (monolithic)** — §6.
8. **MID mode**: specialization variants (command emission, counts-only collect_write), `drawIndirectCount` in the capabilities catalog, per-mode buffer sizing (§0 table), `drawIndexedIndirectCount` draw paths, `MAX_TECHNIQUES` command-buffer padding.
9. **Reinit + settings wiring**: mode plumbed from advanced settings; reinit path re-queries `tic`/submesh capacity.
10. **Cleanup**: delete the four 8 B indirection buffers and dead shader paths.

---

## 4. Validation (run per mode)

- Golden-image compare vs the current renderer (modes must be pixel-identical to each other — this is the invariant test and doubles as the A/B harness).
- Streamed-mesh (`UpdateStreamed`) and entity-churn stress; empty / zero-index submeshes; `indexCount == 0` commands.
- MID-specific: validation layers on (the `maxDrawCount` VUID only fires at record time); technique-buffer padding case (one technique owning nearly all submeshes).
- Pipeline statistics: `VertexShaderInvocations` should drop by `indexRatio / ACMR` — typically **1.5–2.5×** (up to ~3× with cache-optimized assets) in the occluder prepass, depth prepass, and main pass. The existing `GPU_STATS_FLAGS` query pool (`Renderer.cppm`) measures this directly.
- Reinit path: toggle the setting at runtime, confirm buffers are re-sized to the *current* mesh totals, golden image still matches.

---

## 5. MID-mode spec constraints (do not skip)

- `VUID-vkCmdDrawIndexedIndirectCount-maxDrawCount-03143`: `stride × (maxDrawCount − 1) + offset + sizeof(cmd) ≤ buffer size` must hold **at record time for every call**. GPU-computed `tech_offsets` can put a technique's commands near the buffer end, so `maxDrawCount = MAX_TECHNIQUES` requires the technique command buffer to be padded by `MAX_TECHNIQUES` extra 20 B slots (5 KB/frame) — otherwise validation rejects the call whenever a technique starts within 256 commands of the end. The prepass calls (offset 0, `maxDrawCount = submeshCapacity`, buffer 20 B × capacity) satisfy the VUID exactly.
- `drawIndirectCount` must be added to the capabilities catalog (`Feature` enum + `Vulkan12Features`) — it is currently absent entirely.
- The count buffer value is `min(count, maxDrawCount)`; `indexCount == 0` commands are skipped by spec.

---

## 6. Performance / cost summary

Symbols: `N` = total index count, `S` = submeshes, `r` = index/vertex ratio (1.5–2.5), `V/O/D` = visible/occluder/survivor index counts, ACMR = average cache miss ratio (~0.6 optimized assets, ~1.0–1.3 naive).

| Metric | Today | Monolithic (phase 1) | MID (phase 2) |
|---|---|---|---|
| Indirection VRAM/frame | 4 × 8 B × N = **32 B/index** | 8 B/r (entries) + 4 B (draw_indices) + 3 × 4 B (compact copies) ≈ **16–20 B/index** | 8 B/r + 4 B + ~0 (commands) ≈ **12 B/index** |
| expand traffic/index | 12 B (4r + 8w) | ~12 B (4r + 4w + 8/r) — neutral | same |
| Compaction copies/pass | 16 B/index (r+w 8 B) | **8 B/index** | ~0 (20 B/submesh) |
| Copy-traffic savings @ 2 M visible idx | — | ~48 MB/frame saved | ~96 MB/frame saved (≈0.1–0.25 ms mid-range) |
| VS invocations | 1 per occurrence | **÷ r/ACMR ≈ 1.5–3.3×** (same in both modes — the headline win) | same |
| Draw calls | 1 prepass + 1 occluder + 1/technique | unchanged | unchanged |
| Vulkan features | — | none beyond current | `drawIndirectCount` |

Asset-quality dependency: the VS-invocation gain is `r / ACMR` per submesh; the post-transform cache (16–32 entries historically) only converts occurrences → unique vertices well if index order has locality. Unoptimized exports (ACMR ≈ 1.0–1.3) land at the low end (~1.5–2×), cache-optimized meshes at the high end (~2.5–3.3×). The `GPU_STATS_FLAGS` query pool measures the real number per scene.

---

## 7. Decision log

- **Phased, not big-bang**: monolithic compaction ships first (captures the vertex-cache win with one shader rewrite); MID is layered on via specialization constants without reworking expand or the vertex fetch.
- **Mode is an init-time setting, not a runtime toggle**: buffers sized and usage-flagged per mode; changing the mode re-creates the frame ring (partial renderer restart). Only the active mode's pipelines and buffers exist — zero VRAM waste, no hot-path branches, correct usage flags per mode.
- **CullEntry struct unchanged** (`indirOffset` keeps its role as the submesh's index base in `draw_indices`); only its consumers' emission changes per mode. `occlusion_cull.slang` is untouched in both modes.
- The occluder chain (`occluder_select`, `OccluderPrepass`) is a first-class consumer in both modes — it is never left on the deleted occurrence path.
