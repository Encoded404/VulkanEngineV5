# GPU-Driven Rendering — Design

## Core Idea

Upload all geometry to a flat GPU buffer once. A compute shader transforms every vertex from model space to world space (applying the per-instance model matrix) and performs per-vertex frustum culling. The resulting world-space positions are written to an expanded buffer. The depth pre-pass and main pass both read world-space positions and apply the view-projection matrix locally. An occlusion+sort compute pass groups surviving draws by shader technique, enabling one draw call per technique.

The model transform is done once per vertex per frame — not once per vertex per pass. This is a small flop savings but the real benefit is the technique-batched draw generation that enables one draw call per shader regardless of instance count.

## Descriptor Layouts

All shaders share these descriptor sets. Set numbers are fixed across the pipeline.

| Set | Binding | Contents | Stages | Notes |
|-----|---------|----------|--------|-------|
| 0 | 0 | Bindless textures: `sampler2D allTextures[]` | Fragment | Variable count, update-after-bind |
| 1 | 0 | Instance data: `mat4 modelMatrix`, `uint techniqueId` | Vertex, Compute | Per-instance, uploaded every frame by CPU. `techniqueId` selects the pipeline. |
| 2 | 0 | Expanded position + materialId buffer: `vec3 worldPos`, `uint materialId` | Vertex (depth + main) | Written by [1], read-only thereafter. 16 bytes per vertex |
| 2 | 1 | Expanded attribute buffer: `uint packedNormal`, `uint texCoordPacked`, `uint packedTangent` | Vertex (main) | Written by [1], read-only thereafter. 8-12 bytes per vertex |
| 3 | 0 | Camera data: `mat4 view`, `mat4 proj`, frustum planes | Compute [1], Vertex [2] + [4] | UBO, updated every frame. Shared between compute and raster passes |
| 3 | 1 | Mesh metadata: `uint indexCount`, `uint firstIndex`, `int vertexOffset` | Compute [1] | Per-mesh, static after load |

Sets 0 and 1 are bound for the full frame. Set 2 is the expanded geometry output. Set 3 is camera + mesh data, relevant to both compute and raster.

## Buffer Layouts

### Raw Vertex (GPU, static)
```
vec3   position         (12 bytes) — model space
uint32 packed_normal    ( 4 bytes) — octahedral
vec2   texCoord         ( 8 bytes) — float  × 2
uint32 packed_tangent   ( 4 bytes) — octahedral, optional (zero for non-normal-mapped)
uint16 materialId       ( 2 bytes)
uint16 padding          ( 2 bytes)
Total: 32 bytes per vertex
```
Normals and tangents packed via octahedral encoding.

### Expanded Position + MaterialId Buffer (GPU, per-frame 3× ring)
```
// std430 — 16 bytes per vertex
vec3  worldPos;      // bytes 0-11 (world space, after model transform)
uint  materialId;    // bytes 12-15
```
Written by [1] (expand compute), read by [2] (depth pre-pass) and [4] (main pass). World-space position means each vertex's model transform is done exactly once per frame regardless of how many raster passes read it.

### Expanded Attribute Buffer (GPU, per-frame 3× ring)
```
// std430 — 8 bytes per vertex (12 with tangents)
uint packedNormal;      // bytes 0-3  (octahedral)
uint texCoordPacked;    // bytes 4-7  (float2 as two fp16)
uint packedTangent;     // bytes 8-11 (octahedral, optional)
```
Texcoords packed float→fp16 in the expand shader. Unpack in the vertex shader via `unpackHalf2x16`. Per-frame ring.

### Expanded Index Buffer (GPU, per-frame 3× ring)
```
uint32 index   (4 bytes per vertex)
```
Written by [1], read by [2] and [4]. Sequential per instance.

### Indirect Command Buffer (GPU, per-frame 3× ring)
```
VkDrawIndexedIndirectCommand   (20 bytes per instance)
```
Written by [3] (occlusion+sort), read by [4] (main pass). One command per surviving instance, grouped into contiguous technique ranges.

### Raw Geometry Buffer (GPU, single copy)
```
32 bytes per vertex (see layout above)
```
Single shared copy, static after load. Deferred-free on resize.

## Memory Budget

Expanded buffers are the main cost. For a 5M-vertex scene:

| Buffer | Per vertex | Multiplier | Total |
|--------|-----------|---------|-------|
| Position + materialId | 16 B | ×3 ring | 240 MB |
| Attributes | 8 B | ×3 ring | 120 MB |
| Index | 4 B | ×3 ring | 60 MB |
| Indirect commands | 20 B × instances | ×3 ring | ~3 MB (50k instances) |
| Raw geometry (static) | 32 B | ×1 | 160 MB |
| Instance data | ~68 B × instances | ×3 ring | ~10 MB (50k instances) |

**Total: ~590 MB** for expanded + raw buffers at 5M. At 20M this is ~2.2 GB.

Raw geometry is a single shared copy. On resize (new mesh loaded at runtime), the old buffer is deferred-freed 3 frames after replacement — all in-flight readers are guaranteed done by then.

## Flow

```
[Raw Geometry Buffer]
  Vertex format: position (vec3), normal (uint32 octahedral), texcoord (vec2),
                 tangent (uint32 octahedral), materialId (uint16)
  All static geometry lives here permanently. Uploaded once at load time.

[0] CPU: Bounding-Box Frustum Cull
    Cost: ~0.3 ms for 50K instances
    Per-instance bounding box against 6 frustum planes. Instances entirely
    outside are dropped — no instance data uploaded, no GPU compute cycles
    spent. Catches the bulk (50-80%) of off-screen instances.

[1] COMPUTE: Model Transform + Per-Vertex Cull
    Reads:  raw geometry buffer, instance transforms, camera (viewProj)
    Writes: expanded world-space position + materialId buffer
            expanded attribute buffer (packed normal, texcoord, tangent)
            expanded index buffer (sequential indices per instance)
            initial indirect command buffer (one entry per surviving instance)
    One thread per surviving instance. Each thread loops over its instance's
    vertices, transforms from model to world space (model matrix), then to
    clip space (viewProj) for per-vertex frustum culling. Vertices entirely
    outside the frustum are skipped. World-space positions and attributes are
    written to the expanded buffers for every surviving vertex.

[2] DEPTH PRE-PASS (raster, depth-only)
    Reads:  expanded world-space position buffer, instance data (modelMatrix),
            camera (viewProj), initial indirect commands
    Writes: depth attachment
    Vertex: reads worldPos, computes gl_Position = viewProj * modelMatrix *
            vec4(worldPos, 1.0). Model is identity since position is already
            in world space — this becomes gl_Position = viewProj * vec4(worldPos, 1.0).
    Fragment: no color output

[3] COMPUTE: Occlusion Culling + Command Sorting
    Reads:  depth attachment, expanded world-space position buffer,
            initial indirect commands, instance data (technique_id)
    Writes: final indirect command buffer sorted by shader technique
    Uses Hi-Z occlusion testing against bounding spheres. Surviving instances
    are histogrammed by technique_id, prefix-summed, and scattered into a
    sorted indirect buffer — one contiguous range per technique.

[4] MAIN PASS (raster, per technique, color+depth)
    Reads:  expanded world-space position + attribute buffers,
            instance data (modelMatrix), camera (viewProj),
            final indirect commands, depth (load)
    Vertex: same as depth pass — viewProj * vec4(worldPos, 1.0)
    Fragment: decodes packed normal/tangent, indexes bindless textures by
              materialId, computes shading
    One draw call per technique via the sorted indirect commands. Depth is
    loaded (not cleared) from the pre-pass.
```

## Performance Characteristics

All stages run on the same queue. Estimated frame time breakdown at 5M vertices:

| Stage | Time | Notes |
|-------|------|-------|
| [0] CPU cull | ~0.3 ms | Bounding-box against 6 planes. 50K instances |
| [1] Expand + per-vertex cull | ~0.7 ms | One thread per instance. Transforms M + VP per vertex, writes world-space expanded buffers |
| [2] Depth pre-pass | ~0.5 ms | 2M triangles. VP per vertex (~16 flops), no FS |
| [3] Occlusion + sort | ~0.1 ms | Single merged dispatch |
| [4] Main pass | ~1.3 ms | VP per vertex + bindless FS. Early-Z from pre-pass |
| Hi-Z gen | ~0.15 ms | Mip chain from depth attachment (~2.8M pixels at 1080p) |
| OIT resolve | ~0.3 ms | Full-screen resolve (when transparents present) |
| Barriers + binding | ~0.15 ms | Barriers between stages, pipeline binds (~50 techniques) |
| **Total** | **~3.5 ms** | ~280 fps budget on high-end GPU |

**Model transform done once.** The compute shader in step 1 applies the model matrix (M) per vertex. The depth pass and main pass apply only VP — the composited transform from world to clip space. The model matrix multiply (16 flops) is avoided in both raster passes, saving ~32 Mflops at 5M.

For comparison, a simple indirect draw with full vertex shaders (CPU-culled, no pre-pass, no GPU culling):


| Vertex count | Naive (full VS) | GPU-driven | Gap |
|-------------|-----------------|------------|-----|
| 1M | ~1.5 ms | ~2.0 ms | GPU-driven is *slower* — not worth it |
| 5M | ~4.5 ms | ~3.0 ms | ~1.5 ms saved. Competitive. |
| 20M | ~18 ms | ~9 ms | ~9 ms saved. Now it's the only viable path. |
| 50M | ~45 ms | ~20 ms | GPU-driven enables what naive cannot. |

**The crossover is at ~5M vertices.** Below that the GPU-driven pipeline adds complexity for a loss. Above it the savings grow linearly.

**Midrange GPU (RTX 3060-class, ~200 GB/s bandwidth):** Multiply GPU stage times by ~2×. Total at 5M: ~5-6 ms. Still within 60 fps budget but tighter. The crossover shifts to ~8-10M vertices on midrange hardware.

## Optimization Surface

- **Async compute** would overlap [1] with [2] on hardware with dedicated compute queues (most AMD/Nvidia GPUs). The current codebase creates a single queue — worth revisiting when the pipeline is stable.
- **Ring buffer depth** applies to all per-frame buffers (instance data + expanded buffers). Matches `FRAMES_IN_FLIGHT` (3 default). Expanded buffers need the ring because the GPU reads slot N (depth pre-pass) while writing slot N+1 (expand compute). Configurable at startup for VRAM-constrained systems (2×).
- **Instance data upload** currently uses host-coherent memory (`HOST_VISIBLE | HOST_COHERENT`). This is ~0.2 ms for 50K instances over PCIe 4.0. At 200K+ instances, switch to a persistent-mapped ring buffer to avoid per-frame `map`/`unmap` overhead.

## When This Design Wins

This pipeline is designed for scenes with **high geometric density** (millions of unique vertices, tens of thousands of instances) where vertex transform and culling dominate the frame budget. It is **not** optimal for:

- Low-poly scenes (<1M vertices) — CPU culling + simple vertex shaders is faster and simpler.
- Scenes with heavy per-frame geometry streaming — the expanded buffer overhead is wasted.
- GPUs with <4 GB VRAM — the expanded buffer budget is tight at high vertex counts.

## Why This Works

- **Model transform once per vertex per frame.** The compute shader applies the model matrix exactly once. The depth pass and main pass apply only VP — the model multiply (16 flops) is never repeated. Small savings individually, but at 5M+ vertices it keeps the raster passes lean.
- **Culling compounds.** Frustum cull halves geometry → depth pass is fast → occlusion cull halves again → main pass is even faster.
- **One draw call per technique.** The occlusion+sort step groups surviving instances by shader technique, producing one contiguous draw command range per technique. DGC or a CPU loop over ~50 ranges replaces 50K individual draw calls.
- **Minimal CPU involvement.** The CPU writes per-instance data (modelMatrix + techniqueId) each frame, then submits one compute dispatch and the render graph. Everything else happens on-device.
- **Per-vertex materialId** means the main pass batches by fragment shader, not by material. No state splits for texture changes.

## MaterialId vs TechniqueId

Two separate concepts:

| Concept | Storage | Purpose |
|---------|---------|---------|
| `techniqueId` | Instance data (set 1) | Selects the graphics pipeline (vertex + fragment shader pair). Determined by shader identity, not material. |
| `materialId` | Expanded position buffer (set 2 binding 0), per-vertex | Index into the bindless `allTextures[]` array. Carried alongside `worldPos` as a single uint. |

The raw vertex buffer stores `materialId` per-vertex (2 bytes). During expansion (step 1), it is copied into the expanded position buffer alongside the world-space position. The main pass vertex shader passes it to the fragment shader for bindless texture lookup. The depth pass doesn't touch it — it's in the same cache line but never loaded into a register.

Instance data carries only `techniqueId` (for the sorter) and `modelMatrix` (for transform). No per-instance material override — material is purely per-vertex.

## Dynamic Path Vertex Input

Dynamic (skinned) geometry skips the expand shader but still draws in the same main pass. It uses a **separate vertex binding** (binding 1) with its own vertex buffer containing skinned positions and attributes in the raw vertex format. The main pass vertex shader selects input based on draw type via a push constant or draw-parameter flag. Bindless textures (set 0) and the unified depth buffer are shared.

The vertex shader branches: if `is_skinned`, read from binding 1 and compute MVP directly; otherwise read from the expanded buffers (set 2) and apply only VP. The branch cost is negligible.

## Skinned / Dynamic Meshes

Skinned geometry should not run through this pipeline. Skin vertices are a tiny fraction of total scene geometry (a few characters vs. thousands of static objects). Running them through the full GPU-driven flow adds complexity for negligible gain.

**Approach: Two-tier rendering**

| Tier | Geometry | Path |
|------|----------|------|
| Static | Everything else | Full GPU-driven flow (0→4 above) |
| Dynamic | Skinned, animated objects | Skinning compute → direct `vkCmdDrawIndexedIndirect` in main pass |

The dynamic path skips the transform expansion, depth pre-pass, and occlusion culling. It simply runs a skinning compute shader, then draws the results directly alongside the DGC draw. The performance cost is negligible when dynamic instances are <5% of draw calls.

**DGC interaction:** The DGC token stream processes static draws from the sorted indirect buffer. Dynamic draws cannot be embedded in the same DGC token stream because they use a different vertex binding and need a different pipeline or push constant. The fallback path (CPU iterates technique ranges) handles dynamic draws naturally. For the DGC path, dynamic draws are issued as a separate `vkCmdDrawIndexedIndirect` call after the DGC token stream completes.

This avoids duplicating the entire pipeline for a small subset of data. Only optimize further if profiling proves these draws are a bottleneck.

## Fallback Path: No DGC

`VK_EXT_device_generated_commands` is not supported on older GPUs (e.g., AMD GCN1-4 including the RX 560, some mobile GPUs). The pipeline is identical through step 3. Only step 4 changes:

| | DGC available | Fallback |
|---|---|---|
| Step 3 | Compute writes `VkDrawIndexedIndirectCommand` array grouped by technique | Same output |
| Step 4 | `vkCmdExecuteGeneratedCommandsEXT` — GPU processes token stream to issue all draws | CPU iterate ranges: bind pipeline → `vkCmdDrawIndexedIndirect` per range |

The compute shader output is identical in both paths: a flat array of `VkDrawIndexedIndirectCommand`. DGC wraps it in a token stream; the fallback reads it directly. The DGC path adds a small indirection layer (indirect commands layout + token stream) but eliminates the CPU loop over techniques. For scenes with few techniques (<50), the fallback CPU loop overhead is negligible.

## Key Extensions

- **`VK_EXT_device_generated_commands`** (optional) — GPU processes the sorted indirect command buffer from step 3 directly, eliminating the CPU loop over technique ranges in step 4. Not supported on older GPUs (e.g., AMD GCN1-4 like RX 560).
- **`VK_EXT_descriptor_indexing`** — Bindless textures. Already in use, supported on all Vulkan 1.2+ hardware.
- **Hi-Z occlusion** — Generate a hierarchical Z-buffer from the depth pre-pass output. The occlusion compute shader tests bounding boxes against the appropriate mip level.

## CPU Cull (Step 0) Detailed

**Per-instance test:** A loop over all instances with `Transform` + `MeshReference`. For each, load the precomputed axis-aligned bounding box (stored alongside mesh metadata), transform the 8 corners by the instance's model matrix, test against 6 camera frustum planes. If all 8 corners are outside any single plane → culled.

**Implementation:** Straightforward scalar loop with early-out per instance. SIMD via testing 4 or 8 corners at once helps but is not essential — the bottleneck is AABB data cache misses, not ALU.

**Output:** A compacted list of surviving instance indices. Only surviving instances have their `InstanceData` (model matrix + techniqueId + meshId) uploaded to the GPU buffers. This means fewer PCIe bytes and fewer GPU threads in step 1.

This replaces the need for a GPU-side bounding-box cull. The GPU handles per-vertex culling instead.

## CPU→GPU Handoff after Step 0

The CPU cull produces a compacted list of surviving instance indices. This list is used to:

1. **Drive instance data upload:** Only surviving instances have their `InstanceData` (modelMatrix, techniqueId) written to the mapped GPU buffer. The buffer is written as a dense array — thread N in step 1 reads instance data at index N.

2. **Drive dispatch size:** Step 1 dispatches `ceil(surviving_count / 64)` workgroups.

3. **Drive mesh metadata lookup:** Each thread reads the mesh's vertex range (indexCount, firstIndex, vertexOffset) from set 3 binding 1. The mesh metadata is indexed by the thread's assigned instance's mesh — stored in instance data or derived from the mesh array. For the initial implementation, each thread loops over its instance's vertex range directly.

## Frustum Plane Format

Derived from the view-projection matrix on the CPU once per frame. Stored as 6 × `vec4` (normal + distance) in view space at set 3 binding 0:

```
plane[i] = normalize(row[i] + row[3]) for each of the 6 frustum sides
```

or equivalently: extract the 6 plane equations from the combined viewProj matrix via the standard Gribb-Hartmann method. Both the CPU cull and GPU per-vertex cull use the same representation.

## Render Pass Layout

Two separate render passes (not subpasses), because Hi-Z generation in between requires the depth buffer as a shader resource:

| Pass | Type | Color | Depth | Load | Store |
|------|------|-------|-------|------|-------|
| Depth pre-pass (step 2) | Dynamic rendering | None | Write | Clear | Store (for Hi-Z read, then load in main) |
| Hi-Z gen | Compute barrier | — | — | Transition: attachment → shader read | — |
| Main pass (step 4) | Dynamic rendering | Backbuffer | Read | Load | Store |

On tile-based GPUs this forces a depth resolve to memory between passes, which costs bandwidth but is unavoidable for compute-based occlusion. The alternative (subpass with `VK_SUBPASS_CONTENTS_INLINE_AND_SECONDARY_COMMAND_BUFFERS_EXT`) would not allow a compute dispatch between the depth and color subpasses.

## Expanded Buffer Indexing

The expand shader writes world-space positions, materialIds, and attributes into flat, dense buffers (one contiguous range per frame slot). It also writes a per-instance index buffer — sequential `0, 1, 2, ...` for each instance's surviving vertices. The depth pre-pass and main pass use indexed draws (`gl_VertexIndex` via the index buffer) to fetch world-space positions and attributes.

Each instance's draw command carries `firstIndex` (offset into the index buffer) and `vertexOffset` (offset into the expanded position/attribute buffers), both derived from the atomic write cursor at the time the instance's vertices were written.

The occlusion+sort step (step 3) reorders these commands into technique-sorted order without modifying the expanded buffers themselves — it only shuffles the indirect command indices.

## Expand + Model Transform + Per-Vertex Cull (Step 1) Detailed

**Dispatch:** One thread per surviving instance (after CPU cull). Workgroup size 64.

**Per-instance vertex loop:** Each thread loops over its assigned instance's vertices (from the raw vertex buffer range for that mesh). For each vertex:

1. Load raw position (`vec3`).
2. Load instance model matrix.
3. Compute `vec4 worldPos = modelMatrix * vec4(rawPos, 1.0)`.
4. Compute `vec4 clipPos = viewProj * worldPos` for frustum testing.
5. Test clipPos against all 6 frustum planes. If entirely outside, skip this vertex.
6. Write `worldPos.xyz` + `materialId` (from raw vertex) to expanded position buffer.
7. Write packed normal + packed texcoord + packed tangent to expanded attribute buffer.
8. Write sequential index to expanded index buffer.

**Expanded buffer write position — two-pass approach to avoid gaps:** Each thread does two passes over its vertices:

1. **Count pass:** Transform each vertex to world + clip space, test frustum. Count visible vertices in shared memory.
2. **Reserve + write pass:** Atomically add the visible count to a global expanded-buffer write cursor. Write surviving vertices sequentially from that offset. Write the indirect draw command with the correct `firstIndex` and `vertexOffset`.

**Frustum plane format:** 6 × `vec4` (normal + distance), derived from viewProj via Gribb-Hartmann. Passed as push constants.

**When the two-pass cull is worth it:** The per-vertex cull catches edge cases after CPU cull (large objects barely intersecting the frustum). Skip entirely via specialization constant when CPU cull rate is low — fall back to a single-pass expand with no culling test. Both paths share the same shader; the cull is a statically set branch.

**SIMT divergence:** One thread per instance means a very large instance can serialize smaller ones in the same workgroup. In practice the GPU hides this via warp scheduling. Within reasonable instance sizes (<10K vertices), divergence is negligible.

## Hi-Z Generation

A separate compute dispatch after the depth pre-pass. Reads the depth attachment (transitioned to `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL` via barrier). Outputs a mip chain from full resolution down to 1×1. Each mip level is the minimum depth of the 2×2 block above it (min-filter — conservative occlusion). The number of levels is `floor(log2(max(width, height))) + 1`.

## Hi-Z Occlusion + Command Sorting (Step 3) Detailed

Occlusion culling and command sorting are merged into a single compute dispatch to avoid an extra launch and barrier. The algorithm:

1. **Hi-Z test:** One thread per instance that survived frustum cull. Projects the bounding sphere center to clip space, determines mip level from screen-space radius, samples Hi-Z buffer. If depth > stored min depth → occluded; skip all further work for this instance.

2. **Technique histogram:** Instances that pass the occlusion test atomically increment a shared-memory counter for their `technique_id`. With <256 techniques, this is a small array in shared memory — atomics are fast (no global contention).

3. **Prefix sum:** A single workgroup computes output offsets per technique via parallel prefix sum over the 256-element histogram in shared memory.

4. **Scatter:** One thread per surviving instance reads its technique_id, atomically fetches the next slot from a per-technique offset counter (shared memory), and writes its `VkDrawIndexedIndirectCommand` to the final buffer.

The result is a flat buffer sorted by technique: technique 0's commands are contiguous, then technique 1, etc. This same format is consumed both by DGC (wrapped in a token stream) and by the fallback `vkCmdDrawIndexedIndirect` with per-range offsets.

## Cutout Depth Ordering

Cutout draws occur in the main pass after opaque, with depth-test enabled and alpha test. Because cutout skipped the depth pre-pass, the depth buffer at the start of the main pass contains opaque depths only. When a cutout fragment passes the alpha test and writes its depth, subsequent cutout draws in the same pass may see that new depth.

This means: **cutout depth ordering within the main pass is correct for each individual fragment, but the draw order of cutout instances matters.** If two cutout instances overlap, the later one's fragments that pass both the alpha test and the depth test will overwrite the earlier one's fragments — correct for opaque-style depth sorting, but incorrect if the cutout is meant to be "see-through" (the alpha-tested geometry is just holes, not blending). This is the standard behavior for alpha-test materials and matches how every game engine handles cutout in a depth-pre-pass setup.

## Transparency & Cutout

**Cutout materials (alpha test) do not go through the depth pre-pass.** `discard` in the fragment shader disables early-Z and makes the depth buffer unreliable for Hi-Z occlusion. The cost is not the ALU of a float comparison — it's the blown early-Z and frustrated occlusion culling. Cutout gets its own technique pipeline: participates in the transform expansion, frustum cull, **and occlusion cull** (steps 1 and 3), but skips the depth pre-pass (step 2). The depth buffer used for occlusion culling is written by opaque geometry only, so the Hi-Z test against it is valid regardless of whether the instance being tested is opaque, cutout, or transparent.

**OIT: Layered weighted blending** as the default transparency method. Modeled as a generic accumulation buffer:

1. Opaque main pass writes to the backbuffer and depth (loaded from pre-pass). Transparent fragments in the same pass write to a separate accumulation buffer (color + metadata).
2. A resolve pass reads the accumulation buffer and composites into the backbuffer.

This structure makes OIT methods swappable at runtime — weighted, layered, or even brute-force depth-sorted resolve — with zero changes to the rest of the pipeline. The command sorter places transparent draws last and maintains correct per-technique ordering.

**Transparency draw order (from first to last in the main pass):**
1. Opaque (depth pre-pass has already written depth; main pass loads it)
2. Cutout (depth-test enabled, alpha test, writes depth)
3. Opaque + cutout both feed through steps 0-3. The command sorter in step 3 tags draws by transparency class and emits them in the correct order.
4. Transparent / OIT (depth-test enabled, no depth-write, writes to accumulation buffer instead of backbuffer color)
5. OIT resolve pass

## OIT Accumulation Buffer

Transparent fragments write to a separate accumulation target instead of the backbuffer color attachment:

| Target | Format | Contents |
|--------|--------|----------|
| Color accumulation | `RGBA16F` | Weighted color (or raw color for layered OIT) |
| Revealage / metadata | `R16F` or `R32UI` | Transmittance for weighted OIT, depth layer index for layered OIT |
| Depth (shared with opaque pass) | `D32F` | Read-only for transparents, written by opaque + cutout |

The resolve pass reads the accumulation targets and composites into the backbuffer. Swapping OIT method means replacing the resolve shader only.

## Error Handling / Guards

- **Bindless bounds:** The descriptor for `allTextures[]` is allocated with `max_samplers` entries. The fragment shader must guard `if (materialId >= textureCount) { materialId = 0; }` to avoid GPU hangs on bad material data. Texture count pushed as a uniform.
- **Stale expanded buffer data:** Ring-buffered expanded buffers are never cleared between frames. Per-slot write cursors are reset each frame; reads beyond the cursor are out-of-bounds. Debug builds fill with a poison pattern after the cursor to catch stray reads.
- **Technique overflow:** The sorter caps at 256 technique IDs. If exceeded, extra techniques collapse into technique 255 (a fallback shader). A debug overlay counter and validation warning log flag this.

## Decisions

- **Occlusion + sort merged**: A single compute dispatch performs Hi-Z testing and command sorting.
- **Hi-Z generation**: Separate compute pass. Keeps debugging and iteration easier.
- **DGC integration**: Step 3's sorted indirect command buffer feeds into DGC or the fallback `vkCmdDrawIndexedIndirect` path. Start with the fallback.
- **Ring buffer depth**: All per-frame buffers (instance data + expanded buffers) match `FRAMES_IN_FLIGHT` (3 default). The expand shader writes slot N+1 while the depth pass reads slot N.
- **Single queue**: One graphics+compute queue. Async compute is a future optimization.
- **Per-vertex cull optional**: Toggle via specialization constant based on CPU cull rate.
- **Technique ID assignment**: Assigned sequentially at technique registration time. The sorter's 256-technique limit is a hard cap on registered techniques.
- **Transparency**: Layered weighted OIT as default, swappable resolve pass.
- **Cutout**: Separate technique pipeline, skips depth pre-pass only.
- **Dynamic geometry**: Two-tier split. Revisit if skinned instances become a significant fraction of draw calls.
