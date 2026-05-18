# GPU-Driven Pipeline — Integration Plan

## Current State

- **SceneRenderer** (`src/engine/SceneRenderer/SceneRenderer.cpp`): CPU iterates ECS entities, builds `InstanceData` + `MeshData` arrays, uploads them, dispatches `generate_indirect.comp` (trivial copy into indirect command buffer), then draws per-technique via `vkCmdDrawIndexedIndirect`.
- **InstanceData** (`SceneRenderer.cppm:27-31`): `model_matrix`, `material_id`, 3× pad.
- **Graphics pipeline** (`StandardMeshPipeline.cpp`): Set 0 = bindless textures, set 1 = instance data. Vertex shader does full `viewProj * model * position`.
- **Render graph** (`RenderGraphExecution.cpp`): Already supports `QueueType::Compute`, `ResourceKind::Buffer`, `TransientBufferInfo`, passes without `attachment_setup`. Compute dispatches can be added as normal graph passes — they won't trigger `beginRendering`/`endRendering`.
- **BindlessManager**: Fully functional, set 0 binding 0 = `sampler2D allTextures[]`.
- **TechniqueManager**: Assigns sequential `technique_id` at registration.

---

## Phase 1 — World-Space Expand + Depth Pre-Pass

**Goal:** Add the vertex expansion compute shader that transforms vertices to world space and writes expanded buffers. Add a depth pre-pass that applies VP to read world-space positions. The main pass still runs the original vertex shader (reads raw geometry, does full MVP). This establishes the full buffer pipeline and the two-pass render graph.

### New Shaders

| File | Purpose |
|------|---------|
| `shaders/expand.comp` | One thread per instance. Transforms vertices from model to world space (model matrix), then to clip space (viewProj) for per-vertex frustum culling. Writes expanded world-space position + materialId buffer (vec3 + uint = 16 B), expanded attribute buffer (uint packedNormal + uint texCoordPacked + optional uint packedTangent = 8-12 B), expanded index buffer, and initial `VkDrawIndexedIndirectCommand`. Single-pass for now (no two-pass culling). |
| `shaders/depth_world.vert` | Vertex shader for depth pre-pass. Reads `vec3 worldPos` + `uint materialId` from expanded position buffer (set 2 binding 0). Reads instance `modelMatrix` from instance data (set 1 binding 0). Computes `gl_Position = viewProj * vec4(worldPos, 1.0)`. Note: `modelMatrix` is identity since worldPos is already world-space — the depth shader does `viewProj * vec4(worldPos, 1.0)` directly. |
| `shaders/depth_prepass.frag` | `void main() {}` — no color output. |

### New C++

| Module | Purpose |
|--------|---------|
| `DepthPrepassPipeline/` | Pipeline manager for the depth-only pass. Vertex input: reads vec3 position from expanded position buffer (set 2 binding 0, location 0). Also binds instance data (set 1 binding 0) for modelMatrix. Depth write enable, compare eLessOrEqual. |

### Changes to Existing Files

#### `SceneRenderer.cppm`

- **InstanceData** (`line 27`): Change to `mat4 modelMatrix`, `uint techniqueId`. Remove materialId (now per-vertex) and padding.
- **FrameResources** (`line 74`): Add:
  - `GpuBuffer expanded_position_buffer` — `vec3 worldPos + uint materialId` = 16 B/vertex
  - `GpuBuffer expanded_attribute_buffer` — packed normal + texCoord + optional tangent = 8-12 B/vertex
  - `GpuBuffer expanded_index_buffer` — `uint32` = 4 B/vertex
  - Reference to the scene's raw vertex buffer
- **Descriptor layouts:** Add set 2 (expanded position + attribute), shared across compute, depth, and main passes. Add compute-only set 3 (camera + mesh metadata).
- **Pipelines:** Rename `CreateComputePipeline` to `CreateExpandPipeline`. Add `CreateDepthPrepassPipeline`.

#### `SceneRenderer.cpp`

- **Initialize** (`line 53`): Create the expanded buffers (per-frame 3× ring, sized to total scene vertices). Create expand compute pipeline + depth pre-pass pipeline.
- **Expand dispatch**: Bind expand descriptor set, push `viewProj` + frustum planes, dispatch `ceil(surviving_count / 64)`.
- **PrepareCompute** (`line 287`): Upload compacted instance data (modelMatrix + techniqueId for surviving instances). Push camera data.

#### `DefaultRenderer.cpp`

- **Initialize**: Split into two render graph passes:
  1. **Expand compute** — registered as a render graph compute pass (no attachments).
  2. **Depth pre-pass** — depth attachment (clear → store). Uses `depth_world.vert`.
  3. **Main pass** — unchanged for now (still reads raw geometry, full MVP).
- **RenderFrame**: Order: render graph execute (expand → depth pre-pass → main pass).

#### `StandardMeshPipeline.cpp`

- No changes in Phase 1. Main pass unchanged.

---

## Phase 2 — Two-Pass Culling + World-Space Main Pass

**Goal:** Enable per-vertex frustum culling in the expand shader (two-pass approach). Switch the main pass to read expanded world-space buffers and apply VP.

### New Shaders

| File | Purpose |
|------|---------|
| `shaders/main_world.vert` | Replaces `standard_mesh.vert` for the GPU-driven path. Reads `vec3 worldPos` + `uint materialId` from expanded position (set 2 binding 0). Reads packed normal/texCoord/tangent from expanded attributes (set 2 binding 1). Reads instance `modelMatrix` (identity for world-space). Computes `gl_Position = viewProj * vec4(worldPos, 1.0)` and passes materialId + decoded normal/texCoord/tangent to fragment shader. |

### Changes to Existing Files

#### `shaders/expand.comp`

- Add two-pass culling: count (transform M+VP, test), reserve atomic, write.
- Add frustum planes to push constants.
- Add specialization constant to toggle culling.

#### `SceneRenderer.cpp`

- **Main draw path**: Bind expanded position buffer (set 2 binding 0) and expanded attribute buffer (set 2 binding 1). Use `main_world.vert` as vertex shader. Remove raw vertex buffer binding.

#### `StandardMeshPipeline.cpp`

- **Vertex input** (`line 140`): Zero vertex bindings — the vertex shader reads from SSBOs (set 2) instead of vertex input attributes.
- **Pipeline layout**: Append set 2 (expanded buffers). Push constants: `viewProj` only (no model matrix needed in the shader).
- **Instance data** (set 1 binding 0): Still bound for `techniqueId` and compatibility with dynamic path, but the modelMatrix field is unused by the GPU-driven path (position is already world-space).

---

## Phase 3 — Occlusion + Sort

*Same as before — no changes needed for the world-space layout. The occlusion sort reads bounding spheres (world-space) and initial indirect commands, writes sorted indirect commands. Hi-Z generation is unchanged.*

---

## Phase 4 — Cutout + Transparency

*Same as before — no changes needed. The world-space layout doesn't affect the transparency flow.*

---

## Render Graph Pass Order (Final)

```
[Pass 1] Expand compute          — applies M, does VP for culling, writes world-space expanded buffers
[Pass 2] Depth pre-pass          — depth attachment, applies VP to world-space positions
[Pass 3] Hi-Z gen compute
[Pass 4] Occlusion sort compute
[Pass 5] Main pass (opaque)      — applies VP to world-space positions, bindless FS
[Pass 6] Main pass (cutout)      — same as opaque with alpha test
[Pass 7] Main pass (transparent) — writes accumulation buffers instead of backbuffer
[Pass 8] OIT resolve compute
```

## Buffer Layout

The expand shader writes two separate buffers per frame slot:

**Expanded position + materialId buffer** — 16 bytes per vertex:
```
// std430 — 16 bytes per vertex
vec3  worldPos;      // bytes 0-11 (world space, after model transform)
uint  materialId;    // bytes 12-15 (indexes bindless texture array)
```

**Expanded attribute buffer** — 8 bytes per vertex (12 with tangents):
```
// std430 — 8 or 12 bytes per vertex
uint packedNormal;      // bytes 0-3  (octahedral)
uint texCoordPacked;    // bytes 4-7  (two fp16, unpack via unpackHalf2x16)
uint packedTangent;     // bytes 8-11 (octahedral, optional)
```

**Expanded index buffer:** `uint32` (4 bytes per vertex).

**Depth pass reads position (16 B/vertex) + applies VP.** Main pass reads position + attributes (24-28 B/vertex total) + applies VP.

## Buffer Sizing — No Hard Cap

The expanded buffers (position, attribute, index) are sized based on total vertex count of all loaded meshes. **Per-frame 3× ring** — the expand shader writes slot N+1 while the depth pass reads slot N.

On resize (new meshes loaded): each frame slot independently reallocates when recycled. Raw geometry is a single shared 1× buffer with deferred-free on resize (old buffer freed 3 frames after replacement).

The raw vertex buffer is 32 bytes per vertex. The expanded buffers total 28 bytes per vertex (16 + 8 + 4, or 32 with tangents) × 3 ring. For a 10M-vertex scene: 840 MB expanded + 320 MB raw + ~10 MB instance data = ~1.17 GB without tangents.

## Dependency Chain

```
CPU cull → upload instance data
    ↓
[Pass 1] Expand compute — reads: raw vertex, instance data (modelMatrix), camera (viewProj)
    writes: expanded world-space position+materialId, expanded attributes, expanded index, initial indirect
    ↓
[Pass 2] Depth pre-pass — reads: expanded position, instance data, camera (viewProj), initial indirect
    applies: VP per vertex
    writes: depth attachment
    ↓
[Pass 3] Hi-Z gen — reads: depth attachment, writes: Hi-Z image
    ↓
[Pass 4] Occlusion sort — reads: Hi-Z, bounding spheres, initial indirect, instance data (techniqueId)
    writes: sorted indirect buffer
    ↓ (CPU reads technique histogram)
[Pass 5-7] Main pass — reads: expanded position+attributes, instance data, camera (viewProj), sorted indirect, depth
    applies: VP per vertex
[Pass 8] OIT resolve
```
