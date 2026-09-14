# Indexed Drawing Pipeline: Implementation Plan (rev 3)

**Scope:** replace the occurrence-based indirection (`IndirEntry` per index occurrence, single non-indexed `drawIndirect` per pass) with hardware indexed draws, so the GPU's post-transform vertex cache eliminates redundant vertex-shader invocations in the occluder prepass, depth prepass, and main pass.

This revision supersedes rev 2. It keeps rev 2's architecture (hardware indexed draws, 4 B slot indices, MID as a second draw mode behind a specialization constant) and fixes the correctness, capacity, barrier-modelling, and mode/spec plumbing gaps found by auditing the codebase against rev 2. Every change below is normative.

Changes from rev 2, in brief:

1. **Index mapping is corrected.** Rev 2's `draw_indices = entryBase + (rawIndex − baseVertex)` does not match the engine's index convention (indices are stored mesh-local; expand adds `baseVertex` today). The correct mapping is `entryBase + (rawIndex − vertexWindowBase)` over a tight per-submesh window.
2. **`vertexSpan` is embedded in `SubMesh`/`StaticEntry` as a tight window**, not the mesh vertex count. `StaticEntry` grows to **28 B**, not 24 B.
3. **Scene capacity is dynamic.** `SceneRenderer::Initialize` runs *before* any mesh upload, so total index/vertex-span/submesh counts are unknown at init. `EnsureSceneCapacity` grows the frame ring geometrically from `MeshRenderSystem::ProcessFrame`.
4. **Specialization-constant plumbing is an explicit prerequisite** in `PipelineFactory` (with desc-owned storage so hot reload survives).
5. **Render-graph changes live in `Renderer.cpp`**, not the (dead) `passes/*.cpp` `Setup()` methods, and require a new `IndexInput → eVertexInput / eIndexRead` intent.
6. **`drawIndirectCount` is `Optional`** with a runtime fallback to monolithic.
7. **MID collect is a submesh-count/prefix-sum/command-emission algorithm**, not a field swap.
8. **Occluder selection budget and occluder command count are separate counters** in MID.

**Non-goals / unchanged:** occlusion culling decision logic, Hi-Z, mesh upload paths, bindless vertex fetch, technique system, render-graph topology. Vertex-cache optimization of asset index buffers (e.g. meshoptimizer) remains a non-goal — see §8.

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
- **Changing the mode is supported at runtime** via `SceneRenderer::Reinitialize(DrawMode)` (§2), which device-idles, tears down and re-creates the mode-dependent frame buffers with the active mode's usage flags and sizes, and rebuilds only the `kCompactionMode`-parameterized compute pipelines. Descriptor set layouts/pools and technique graphics pipelines are mode-independent and are reused. Mesh-side data (`StaticEntry`, asset index/vertex buffers, `vertex_entries` inputs) is mode-independent and untouched.
- **Only the active mode's pipelines are created.** The two variants come from the same shader sources via one Slang specialization constant (`kCompactionMode`); the inactive variant is never compiled into a pipeline. ShaderWatcher hot-reload keeps working (both variants derive from one source file).
- **Buffers are sized and usage-flagged per mode** (§2) — no aliasing, no dual-purpose buffers. The validation layer then catches any accidental cross-mode use (e.g. drawing from a buffer lacking `eIndexBuffer`).

---

## 1. Data model

### Today

`expand` flattens every submesh's index buffer into `indirection_buffer` — one 8 B `IndirEntry {vertexId, submeshId}` **per index occurrence** (`totalIndexCount × 8` B per frame). The occluder prepass draws from `occluder_indirection_buffer` (compacted by `occluder_select`), the depth prepass from `depth_indirection_buffer` (compacted by `pre_cull`), the main pass from `compacted_indirection_buffer` (compacted by collect). **Four** buffers of 8 B × totalIndexCount are allocated per frame. Every occurrence is a separate vertex-shader invocation with zero reuse.

### Shared substrate (both modes)

| Buffer | Content | Size | Written |
|---|---|---|---|
| `vertex_entries` | one `IndirEntry {packedVid, submeshId}` per vertex **slot in the submesh's tight window** — same struct, indexed by *slot* instead of occurrence | 8 B × Σ vertexSpan | per frame (expand) |
| `draw_indices` | values = absolute slots into `vertex_entries`. Per occurrence: `entryBase + (rawIndex − vertexWindowBase)` | 4 B × totalIndexCount | per frame (expand) |
| cull entries (existing) | struct unchanged in size/order: `{indexBase, indexCount, techniqueId, pad}` — `indexBase` is the *slot base of the submesh's indices in `draw_indices`* (same value expand allocated before). Field renamed from `indirOffset` for clarity | 16 B × submeshes | per frame (expand) |
| `expand_counter` | 2 × u32 atomic counters (`entryBase`, `indexBase`); zeroed by the CPU each frame (alongside the existing counter uploads in `PrepareCompute`) | 8 B | per frame |

Key properties:

- **No dedup anywhere.** The tight vertex window `[vertexWindowBase, vertexWindowBase + vertexSpan)` is the unique set consumed by this submesh; expand's occurrence flattening was the only duplication and it is removed.
- **Per-vertex submesh info is preserved** — it lives in the entries, fetched by index value. Culling granularity, technique handling, and the VS pull code are structurally unchanged.
- **Occlusion chain untouched**: `occlusion_cull.slang` (zeroes `indexCount`), the `CullEntry.pad` occluder bit, and the technique flag gating are identical in both modes.

### Vertex window (`vertexWindowBase` / `vertexSpan`)

The engine stores indices **mesh-local** (`MeshUploadManager` copies `data.indices` verbatim) and converts to an absolute vertex at expand time with `rawIndex + baseVertex`, where `baseVertex = vertex_alloc.offset / sizeof(Vertex)`. To avoid pushing an entry for every vertex of a multi-submesh mesh (which would be O(K·V)), each `SubMesh` carries a tight window:

```
vertexWindowBase = min over the submesh's indices
vertexSpan       = max − min + 1
```

computed once at upload from the CPU index data (`EnsureSubmeshBounds` path). The `vertex_entries` slot `j ∈ [0, vertexSpan)` corresponds to absolute vertex `baseVertex + vertexWindowBase + j`, and index replay is `entryBase + (rawIndex − vertexWindowBase)`.

New static per-submesh data: `vertexWindowBase` and `vertexSpan`, carried in `StaticEntry`. The 24-bit `baseVertex` packing limit is unchanged and now also bounds `baseVertex + vertexWindowBase + vertexSpan`; that invariant is enforced as an upload-time error (never silently masked).

### Per-mode buffers (replacing the four 8 B × N indirection buffers)

| Destination | Monolithic | MID |
|---|---|---|
| main-pass output | `main_compact_indices`: 4 B × N, `{eStorageBuffer \| eIndexBuffer}` | `main_commands`: 20 B × S, `{eStorageBuffer \| eIndirectBuffer}`; `regionBase`: 4 B × `MAX_TECHNIQUES` host-visible (CPU prefix sum) |
| depth-prepass survivors | `depth_compact_indices`: 4 B × N, `{eStorageBuffer \| eIndexBuffer}` | `depth_commands` (20 B × S) + `depth_command_count` (4 B) |
| occluder-prepass set | `occluder_compact_indices`: 4 B × N, `{eStorageBuffer \| eIndexBuffer}` | `occluder_commands` (20 B × S) + `occluder_command_count` (4 B) |
| `draw_indices` | `{eStorageBuffer}` (compaction source only; still `eTransferDst`) | `{eStorageBuffer \| eIndexBuffer}` (bound for draws) |
| `technique_draw_commands` | 20 B × `MAX_TECHNIQUES` | not allocated (collect-write is monolithic-only; MID main pass uses `main_commands`) |
| `tech_counts_buffer` | allocated but unused | gains `eIndirectBuffer`; written by the MID compact pass |
| `depth_draw_command` / `occluder_draw_command` | 20 B `DrawIndexedIndirectCommand` (host-visible, indirect), word 0 accumulates | not allocated |

In monolithic mode the prepass/occluder command buffers are CPU-preinitialized to `{indexCount = 0, instanceCount = 1, firstIndex = 0, vertexOffset = 0, firstInstance = 0}`; pre-cull / occluder-select accumulate into word 0 exactly as they do today into `prepassCounter`/`occluderCounter`.

In MID mode the occluder command count (`occluder_command_count`) is distinct from the occluder **selection budget** counter (`occluder_count_buffer`); the former counts emitted occluder commands for `drawIndexedIndirectCount`, the latter enforces `kMaxOccluders`.

Deleted in both modes: `indirection_buffer`, `compacted_indirection_buffer`, `depth_indirection_buffer`, `occluder_indirection_buffer` — **four** buffers of 8 B × totalIndexCount.

---

## 2. Per-component changes

### Shaders

A single specialization constant (`kCompactionMode`, 0 = monolithic, 1 = MID) selects the emission variant in the compaction/collect shaders:

```slang
[[vk::constant_id(0)]] const uint kCompactionMode = 0;
```

Both variants compile from the same source; only the active mode's pipeline is created at init. The offline compiler (`slang-spirv-compiler`) lowers this to a SPIR-V `OpSpecConstant`, so no CMake/helper changes are needed. `ComputePipelineDesc` gains `spec_entries` (`std::vector<vk::SpecializationMapEntry>`) and `spec_data` (`std::vector<std::byte>`), both owned by the desc stored in the per-pipeline `optional<ComputePipelineDesc>` so hot reload passes them to `vk::SpecializationInfo` unchanged.

**`expand.slang`** — the only substantial rewrite, identical in both modes:
- Allocate `entryBase` via `InterlockedAdd(counter[0], vertexSpan)` and `indexBase` via `InterlockedAdd(counter[1], indexRange)`.
- Write `vertex_entries[entryBase + j] = {(vertexBufSlot << 24) | (baseVertex + windowBase + j), id}` for `j < vertexSpan`. No index reads needed for this.
- For `i < indexRange`: read `rawIndex` from `indexBuffers[slot][offset + i]`, write `draw_indices[indexBase + i] = entryBase + (rawIndex − windowBase)`.
- Write `cullEntries[id] = {indexBase, indexRange, techId, 0}`.
- Delete the per-occurrence `IndirEntry` stamping loop.

Do **not** have the compactors read asset index buffers and convert on the fly: three consumers (collect, pre-cull, occluder-select) would each redo the conversion; writing `draw_indices` once and copying from it wins.

**`main_indir.slang`** — drop `SV_StartVertexLocation`; fetch `vertexEntries[vertexId]` (indexed draws give `SV_VertexID = slot + vertexOffset`, `vertexOffset = 0`).

**`depth_indir.slang`** — binding rename only: set 3 binds `vertex_entries`; fetch is `vertexEntries[vertexId]`.

**`pre_cull.slang`** and **`occluder_select.slang`** — decision logic (frustum, Hi-Z, OBB tests, occluder bit, flag gating) is **unchanged**; only the survivor/occluder emission changes:
- *Monolithic*: copy `draw_indices[indexBase + i]` (uint, 4 B) into the destination index buffer instead of `IndirEntry` (8 B). Counter semantics unchanged.
- *MID*: emit one `DrawIndexedIndirectCommand {indexCount, 1, indexBase, 0, 0}` per survivor/occluder + `InterlockedAdd(count, 1)`.

**`collect_count_compact.slang`** — *Monolithic*: count pass unchanged (`InterlockedAdd(inter[tech].count, entry.indexCount)`); compact pass copies 4 B indices instead of 8 B entries. *MID*: the main-pass compact reads `regionBase[tech]` (CPU-prefixed, see below), appends one 20 B command per alive submesh at `regionBase[tech] + InterlockedAdd(aliveCount[tech], 1)`, and the count pass is removed (the count buffer is GPU-written by the compact pass). `regionSize[tech] = submeshCount[tech]`, so every region is exactly its worst case.

**`collect_write.slang`** — **monolithic only** (no specialization constant): `DrawIndexedIndirectCommand(count, 1, entryOffset, 0, 0)` per technique (`vertexCount/firstVertex` → `indexCount/firstIndex`). It is not created or dispatched in MID.

**MID alive-count publishing** — the compact pass writes `tech_counts[t]` directly with `InterlockedAdd(techCounts[entry.techniqueId], 1u)` (every alive submesh adds exactly once), which is race-free and removes the need for a separate epilogue dispatch. `tech_counts_buffer` with `eIndirectBuffer` is then bound straight to `drawIndexedIndirectCount`. `tech_offsets` is deleted — it was dead in both modes.

### CPU per-technique command regions (MID main pass)

`vkCmdDrawIndexedIndirectCount`'s `offset` argument is a **CPU command-recording value**; it cannot be GPU-written in the same frame. The main pass therefore uses a **CPU-computed prefix sum** computed during gather (the CPU already knows each submesh's technique):

- For each technique `t`, `submeshCount[t]` = number of submeshes whose material maps to `t`.
- `regionBase[t] = prefix_sum(submeshCount[0..t-1])`; the main command buffer has `totalSubmeshes` slots.
- `regionBase[]` is uploaded once per capacity/topology change to a host-visible `u32 × MAX_TECHNIQUES` buffer; it is not per-frame data.
- Render issues `drawIndexedIndirectCount(main_commands, regionBase[t]*20, submeshCount[t], 20, tech_counts_buffer, t*4)`.
- The VUID holds **by construction**: `20 × (submeshCount[t] − 1) + regionBase[t] × 20 + 20 = 20 × regionBase[t+1] ≤ 20 × S`. No padding hack, no readback.
- The compact pass needs only `regionBase[t]` (read) and `aliveCount[t]` (atomic, zeroed per frame). No per-submesh command slot is required — append order within a region is irrelevant.

**`occlusion_cull.slang`** — no behavioral changes; only the `CullEntry.indirOffset → indexBase` field rename.

### C++ — `VulkanCapabilities.cppm` / `.cpp` (MID mode only)

- Add `Feature::DrawIndirectCount` to the enum + `kFeatureCatalog` as **`Optional`** (`{ "drawIndirectCount", Optional }`), enable `Vulkan12Features::drawIndirectCount` when supported, and map it in `GetSupportedFeature` / `GetRequestedFeature` / `SetRequestedFeature`.
- Because features are requested at device creation before any mode is known, `Required` would reject monolithic-capable devices. The renderer queries `caps.IsFeatureEnabled(Feature::DrawIndirectCount)`, selects the effective mode, and logs/warns on fallback.
- Monolithic needs nothing new (`drawIndexedIndirect` is core 1.0). No `drawIndirectFirstInstance` needed (firstInstance is always 0).

### C++ — `SceneRenderer.cpp` / `.cppm`

- `DrawMode` enum member (from engine config), threaded into `Initialize`.
- `SceneCapacity {index_count, vertex_span, submesh_count}` and `EnsureSceneCapacity(...)`; `Initialize` takes an initial capacity and grows geometrically (×2) from `MeshRenderSystem::ProcessFrame` after totals are known (§3). Growth device-idles and re-creates the mode buffers.
- Buffers per §1 table, allocated per mode with per-mode usage flags. `vertex_entries`, `draw_indices`, `expand_counter` exist in both.
- Descriptor sets: **one** shared indirection set bound to `vertex_entries`, used by the depth, occluder, and main passes. The indirection layout is unchanged (single VS storage buffer at binding 0). All buffer descriptor writes are performed in `PrepareCompute`/`Dispatch*` each frame (never only once at init), so capacity growth and mode changes are transparent.
- Compaction pipelines: created for the active mode only (`kCompactionMode` on `pre_cull`, `occluder_select`, `collect_count_compact`, `collect_write`).
- **`Reinitialize(DrawMode)`**: device idle → destroy and re-create mode-dependent frame buffers with the new usage flags/sizes → rewrite the `kCompactionMode` spec value on the stored pipeline descs and rebuild those pipelines. Descriptor layouts/pools and technique pipelines are untouched.

### C++ — `SceneRendererFrame.cpp`

- **Monolithic**:
  - `DepthPrepass` / `OccluderPrepass`: `bindIndexBuffer(depth/occluder_compact_indices, 0, eUint32)` + one `drawIndexedIndirect` from the GPU-written 20 B command.
  - `Render`: `bindIndexBuffer(main_compact_indices)`; per technique one `drawIndexedIndirect(technique_draw_commands, t * 20, 1, 20)` — command = `{count, 1, offset, 0, 0}`.
  - Compaction shaders copy 4 B `uint`s from `draw_indices` — halved copy traffic, all copy loops and culling logic keep their structure.
- **MID**:
  - Prepass: `drawIndexedIndirectCount(commands, 0, submeshCapacity, 20, count_buffer, 0)`.
  - `Render`: `drawIndexedIndirectCount(main_commands, regionBase[t] * 20, submeshCount[t], 20, tech_counts_buffer, t * 4)` — `regionBase`/`submeshCount` are CPU-computed prefix sums (see above), so the VUID holds exactly with no padding.
  - `indexCount == 0` commands (culled submeshes, empty techniques) are skipped by spec.
- **Barriers (both modes)**: `draw_indices` and any compact-index destination: `eShaderWrite → eIndexRead` / `ComputeShader → VertexInput`; `vertex_entries`: `eShaderWrite → eShaderRead` / `ComputeShader → VertexShader`; command/count buffers: `eShaderWrite → eIndirectCommandRead` / `ComputeShader → DrawIndirect`. Wire via new render-graph buffer resources and the new intent in `Renderer.cpp` / `RenderGraphTypes.cppm` (§2, WS4), not the dead `passes/*.cpp` `Setup()` methods.

### C++ — `MeshGatherSystem.cpp` / mesh data

- `SubMesh` gains `vertexWindowBase` and `vertexSpan`, computed once from the CPU index data in `EnsureSubmeshBounds` (and preserved through the `index_offset` adjustment in `GameEngine`/`MeshCache`).
- `StaticEntry` gains `vertex_window_base` and `vertex_span`: **28 B** (`static_assert(sizeof(StaticEntry) == 28)`); `compact_static` block config `20 → 28`.
- `MeshGatherSystem::ProcessFrame` accumulates `total_index_count`, `total_vertex_span`, `total_submeshes` and calls `SceneRenderer::EnsureSceneCapacity` before `PrepareCompute`.
- `UpdateStreamed` refreshes CPU index data each frame; the vertex window is computed at `RegisterStreamed` and **topology is required to be stable** (index count/per-submesh ranges unchanged). A topology change requires re-registration. No changes to `MeshUploadManager`'s upload mechanics.

### C++ — `MeshUploadManager.*` / `SubMesh`

- Per-frame totals (`Σindex_count`, `Σvertex_span`, `submesh_count`) are accumulated in `MeshGatherSystem::ProcessFrame` as it walks the frame's submeshes and fed straight into `EnsureSceneCapacity`, so no separate registry walk is needed.
- The 24-bit invariant `baseVertex + vertexWindowBase + vertexSpan < 2^24` is checked where `baseVertex` actually becomes known — the gather, when the vertex-heap allocation offset is applied. It asserts in debug and logs a loud error once in release (never silently masked).

### Deleted (both modes)

`indirection_buffer`, `compacted_indirection_buffer`, `depth_indirection_buffer`, `occluder_indirection_buffer` (8 B × N each), the per-occurrence stamping loop, and the stale duplicate draw path in `passes/MainPass.cpp` (the live path is `SceneRenderer::Render`).

---

## 3. Capacity model (new)

`SceneRenderer::Initialize` is called from `GameEngine::InitRenderer` **before** `UploadScene`/`RequestGpuResidency`, so no real totals are available at init. There is no settings system and no reinit path today. Therefore:

- `Initialize(..., SceneCapacity initial_capacity)` allocates the frame ring at `max(initial, 1)`.
- `MeshRenderSystem::ProcessFrame` accumulates the frame's true totals across static + dynamic entities and calls `SceneRenderer::EnsureSceneCapacity(index_count, vertex_span, submesh_count)`.
- `EnsureSceneCapacity` returns immediately when the current capacity covers the request; otherwise it device-idles, sets capacity to `max(needed, current × 2)`, and re-creates all capacity-dependent buffers (entries, indices, per-mode compact/command buffers, `technique_draw_commands`) for every frame in the ring. Descriptors are rewritten per frame, so no extra descriptor work is needed.
- Mode changes reuse the same path with the target mode's usage flags (`Reinitialize`).

This is mandatory for streaming, entity churn, and any real scene; fixed init-time buffers cannot work.

---

## 4. File change list

| File | Change |
|---|---|
| `src/engine/shaders/expand.slang` | rewrite: entries + `draw_indices` replay + two counters; delete entry stamping |
| `src/engine/shaders/main_indir.slang` | drop `baseVertex`/`SV_StartVertexLocation`; fetch `vertexEntries[vertexId]` |
| `src/engine/shaders/depth_indir.slang` | binding rename to `vertexEntries` |
| `src/engine/shaders/pre_cull.slang` | index copy (mono) / command emission (MID), `kCompactionMode`, `indexBase` rename |
| `src/engine/shaders/occluder_select.slang` | same split for the occluder leg |
| `src/engine/shaders/collect_count_compact.slang` | 4 B index copy / alive-submesh count + command emission, spec constant |
| `src/engine/shaders/collect_write.slang` | indexed command fields; monolithic-only (no spec constant, not dispatched in MID) |
| `src/engine/shaders/occlusion_cull.slang` | `indirOffset → indexBase` rename only |
| `src/engine/render/SceneRenderer.cppm` | `DrawMode`, `SceneCapacity`, frame-struct buffer members, feature check, config plumbing |
| `src/engine/render/SceneRenderer.cpp` | per-mode buffer allocation + usage flags + capacity growth, descriptor bindings, pipeline creation for active variant, `Reinitialize` |
| `src/engine/render/SceneRendererFrame.cpp` | `DepthPrepass`/`OccluderPrepass`/`Render` indexed draws, `DispatchCollect` modes, per-frame descriptor writes, counter zeroing |
| `src/engine/render/Renderer.cpp` | render-graph buffer resources + `IndexInput` read declarations (mode-independent) |
| `src/shared/render_graph/RenderGraphTypes.cppm` | `PipelineStageIntent::IndexInput → eVertexInput`; `eIndexRead` access |
| `src/engine/render/PipelineFactory.cppm` / `.cpp` | specialization-constant fields in `ComputePipelineDesc` + `pSpecializationInfo` in `CreateCompute` |
| `src/engine/render/MeshGatherSystem.cpp` | `StaticEntry` + window/span (28 B), mirror + `static_assert`, totals + `EnsureSceneCapacity` |
| `src/engine/assets/MeshTypes.cppm` | `SubMesh` window/span fields |
| `src/engine/gpu/MeshData.cppm` | compute window/span in `EnsureSubmeshBounds` |
| `src/engine/assets/MeshCache.cpp` | no change needed: the window is computed from index *values*, which the `index_offset` adjustment does not alter; `EnsureSubmeshBounds` still (re)computes it here |
| `src/engine/render/MeshUploadManager.*` | no change needed: totals and the 24-bit invariant are handled in `MeshGatherSystem` where the heap offset is known; `RegisterStreamed`/`UploadPersistent` already call `EnsureSubmeshBounds` so streamed windows are populated |
| `src/engine/core/GameEngine.cpp` / `EngineContext.cppm` | `DrawMode` in `GameConfig`, initial capacity, capacity call |
| `src/backend/vulkan/VulkanCapabilities.cppm` / `.cpp` | `drawIndirectCount` optional feature + mapping |
| `src/engine/render/passes/MainPass.cpp` | delete the stale duplicate draw path |

---

## 5. Implementation order

The work is one production change set; the ordering below is for review, not separate shippable phases.

1. **Data/capacity (WS1)**: `SubMesh` window/span → `StaticEntry` 28 B → mesh totals → `EnsureSceneCapacity` → `DrawMode` config + `RenderPipeline` resources.
2. **Multi-indirect-draw must not use pre-1.2 atomic-empty-tile semantics inadvertently**: not applicable (no atomic ops on commands).
3. **Buffers & descriptors (WS2)**: `vertex_entries`, `draw_indices`, `expand_counter`, per-mode destinations with per-mode usage flags; shared indirection set; per-frame descriptor writes; spec-constant plumbing; `Reinitialize`.
4. **`expand.slang`** rewrite (entries + index replay + counters).
5. **Vertex shaders**: `main_indir` baseVertex removal; `depth_indir` binding rename.
6. **Compaction shaders + collect_write** with `kCompactionMode` (monolithic variants first).
7. **Draw calls + barriers (WS4)**: `bindIndexBuffer` + `drawIndexedIndirect`; render-graph `IndexInput` resources; counter zeroing in `PrepareCompute`.
8. **MID**: command emission (append at `regionBase[tech] + aliveCount`), CPU per-technique region prefix sum, `DrawIndirectCount` capability, per-mode sizing, `drawIndexedIndirectCount`.
9. **Validation (WS6)**.
10. **Cleanup**: delete the four 8 B buffers, dead shader paths, stale `MainPass` draw code.

---

## 6. Validation (run per mode)

- Golden-image compare vs. the current renderer (modes must be pixel-identical to each other — this is the invariant test and doubles as the A/B harness).
- Streamed-mesh (`UpdateStreamed`) and entity-churn stress; empty / zero-index submeshes; `indexCount == 0` commands.
- Capacity growth mid-session: confirm buffers are re-sized to the *current* totals and the golden image still matches.
- MID-specific: validation layers on (the `maxDrawCount` VUID only fires at record time); technique-buffer padding case (one technique owning nearly all submeshes).
- Pipeline statistics: `VertexShaderInvocations` should drop by `indexRatio / ACMR` — typically **1.5–2.5×** (up to ~3× with cache-optimized assets) in the occluder prepass, depth prepass, and main pass. The existing `GPU_STATS_FLAGS` query pool (`Renderer.cppm`) measures this directly.
- Reinit path: toggle the setting at runtime, confirm buffers are re-sized to the *current* mesh totals, golden image still matches.

---

## 7. MID-mode spec constraints (do not skip)

- `VUID-vkCmdDrawIndexedIndirectCount-maxDrawCount-03143`: `stride × (maxDrawCount − 1) + offset + sizeof(cmd) ≤ buffer size` must hold **at record time for every call**. The main pass satisfies it by construction: `offset = regionBase[t]*20`, `maxDrawCount = submeshCount[t]`, `regionBase[t+1] = regionBase[t] + submeshCount[t]`, so the left side equals `20 × regionBase[t+1] ≤ 20 × S`. The prepass calls (`offset 0`, `maxDrawCount = submeshCapacity`, buffer `20 B × capacity`) satisfy it exactly. No padding is required. `regionBase`/`submeshCount` are CPU-computed prefix sums, never GPU-written.
- `drawIndirectCount` is an **Optional** capability: the renderer uses MID only when `caps.IsFeatureEnabled(Feature::DrawIndirectCount)`, otherwise logs and falls back to monolithic.
- The count buffer value is `min(count, maxDrawCount)`; `indexCount == 0` commands are skipped by spec.

---

## 8. Performance / cost summary

Symbols: `N` = total index count, `S` = submeshes, `r` = index/vertex ratio (1.5–2.5), `V/O/D` = visible/occluder/survivor index counts, ACMR = average cache miss ratio (~0.6 optimized assets, ~1.0–1.3 naive).

| Metric | Today | Monolithic (phase 1) | MID (phase 2) |
|---|---|---|---|
| Indirection VRAM/frame | 4 × 8 B × N = **32 B/index** | 8 B/r (entries) + 4 B (draw_indices) + 3 × 4 B (compact copies) ≈ **16–20 B/index** | 8 B/r + 4 B + ~0 (commands) ≈ **12 B/index** |
| expand traffic/index | 12 B (4r + 8w) | ~12 B (4r + 4w + 8/r) — neutral | same |
| Compaction copies/pass | 16 B/index (r+w 8 B) | **8 B/index** | ~0 (20 B/submesh) |
| Copy-traffic savings @ 2 M visible idx | — | ~48 MB/frame saved | ~96 MB/frame saved (≈0.1–0.25 ms mid-range) |
| VS invocations | 1 per occurrence | **÷ r/ACMR ≈ 1.5–3.3×** (same in both modes — the headline win) | same |
| Draw calls | 1 prepass + 1 occluder + 1/technique | unchanged | unchanged |
| Vulkan features | — | none beyond current | `drawIndirectCount` (optional) |

Asset-quality dependency: the VS-invocation gain is `r / ACMR` per submesh; the post-transform cache (16–32 entries historically) only converts occurrences → unique vertices well if index order has locality. Unoptimized exports (ACMR ≈ 1.0–1.3) land at the low end (~1.5–2×), cache-optimized meshes at the high end (~2.5–3.3×). The `GPU_STATS_FLAGS` query pool measures the real number per scene.

---

## 9. Decision log

- **Phased, not big-bang**: monolithic compaction ships first (captures the vertex-cache win with one shader rewrite); MID is layered on via specialization constants without reworking expand or the vertex fetch.
- **Mode is an init-time setting with a supported runtime reinit**: buffers sized and usage-flagged per mode; `Reinitialize` re-creates the frame ring (partial renderer restart). Only the active mode's pipelines and buffers exist — zero VRAM waste, no hot-path branches, correct usage flags per mode.
- **Tight vertex window, not mesh vertex count**: multi-submesh meshes would otherwise pay O(K·V) entries. Computed once at upload; topology stability is a documented requirement of the streamed path.
- **Index convention corrected**: `draw_indices = entryBase + (rawIndex − vertexWindowBase)`; entries cover the tight window and store absolute vertex `baseVertex + vertexWindowBase + j`.
- **Dynamic capacity**: capacity is grown from the gather pass because init precedes upload; all buffer descriptors are written per frame so growth/mode changes need no descriptor surgery.
- **CullEntry struct unchanged in layout** (`indexBase` keeps its role as the submesh's index base in `draw_indices`); only its consumers' emission changes per mode. `occlusion_cull.slang` is untouched apart from the field rename.
- The occluder chain (`occluder_select`, `OccluderPrepass`) is a first-class consumer in both modes — it is never left on the deleted occurrence path.
- **`collect_write` is monolithic-only.** MID publishes its per-technique alive count from the parallel compact pass (`InterlockedAdd(techCounts[tech], 1u)`), which is race-free and removes the need for a mode branch, a dummy command binding, and the vestigial `tech_offsets` buffer.
- **Barriers are declared at graph build** with a new `IndexInput → eVertexInput / eIndexRead` intent, in `Renderer.cpp` (the live graph), not the dead `passes/*.cpp` setup methods.
