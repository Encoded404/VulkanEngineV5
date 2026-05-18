# VulkanEngineV5 — Renderer Architecture Plan

## 1. Architecture Layers

```
app/              — The game itself
engine/           — Reusable rendering + systems code, configurable per-game
backend/          — Vulkan API foundation, changed very rarely
```

---

## 2. Current State (May 2026)

### What exists

| Module | Purpose |
|---|---|
| `Camera` component | Position, target, FOV, ortho/perspective, view/proj matrix methods |
| `Transform` component | Position, Y-rotation, scale |
| `MeshRenderer` component | `visible` flag only — no mesh reference, no material |
| `SceneLoader` | Mesh (.bin) + texture loading, converts to AOS vertices, uploads to GPU |
| `DefaultRenderer` | Wraps RenderPipeline + FrameRenderer + PipelineManager, per-frame draw |
| `MeshRendererSystem` | Iterates MeshRenderer entities, push-constant MVP, drawIndexed |
| `StandardMeshPipeline` | Single vk::Pipeline with PipelineConfig (cull, depth, blend, topology) |
| `RenderPipeline` | Render graph: build → compile → execute with automatic barriers |
| `FrameRenderer` | Backbuffer/depth state management, ImGui overlay, present barrier |
| `GpuBuffer` | Generic GPU buffer — any usage flags, any memory properties |
| `GpuDescriptorSet` | Descriptor allocation + update (texture + buffer bindings) |
| `ResourceSystem` | CPU-side Resource<T> with ResourceHandle<T>, ref-counting, async loading |
| `ShaderLoader` | SPIR-V binary loading (stage-agnostic) |
| `Input` | Action-based input system |
| `ImGuiSystem` | ImGui integration |
| ECS | Entity + Component + ComponentRegistry (typed pools, ForEach, async update) |
| Render graph | QueueType::Graphics/Compute/Transfer, pipeline stage tracking, auto barriers |

### Current render flow (per frame)

```
1. Set backbuffer + depth initial states
2. Begin dynamic rendering (clear color + depth)
3. MeshRendererSystem::RecordAllMeshDraws:
     ForEach<MeshRenderer>:
       BuildPushConstants(transform, view, proj) → MVP
       drawIndexed (same mesh for every entity)
4. ImGui overlay
5. Present barrier
```

### Key limitation

Every entity draws the **same mesh** with the **same pipeline**. No per-entity mesh, no material system, no culling, no sorting.

---

## 3. Completed Refactoring (this session)

| Change | File |
|---|---|
| Camera component | `engine/Components/Camera.cppm` — position, FOV, ortho/perspective |
| SceneLoader (moved from app) | `engine/SceneLoader/SceneLoader.*` — generic load + upload |
| DefaultRenderer | `engine/DefaultRenderer/DefaultRenderer.*` — wraps pipeline + frame lifecycle |
| Game class (moved from main hooks) | `app/Game.*` — all state encapsulated, main.cpp is ~25 lines |
| MeshRendererSystem accepts view+proj | No longer hardcoded camera matrices |
| PipelineConfig | Exposes cull/depth/blend/topology — `StandardMeshPipeline` namespace |
| Old DemoSceneRenderer | Deleted — replaced by engine's SceneLoader |
| Renderer architecture plan | This document |

---

## 4. Long-Term Architecture Vision

### Principles

1. **Vulkan 1.3 core** — dynamic rendering, synchronization2. Extended with `VK_EXT_descriptor_indexing`, `VK_KHR_buffer_device_address`, `multiDrawIndirect`, `VK_EXT_device_generated_commands`.
2. **GPU-driven rendering** — CPU sends one (or few) indirect dispatch commands per frame. All culling, sorting, and command generation happens on the GPU.
3. **Compute-first vertex expansion** — all geometry is fully transformed to view space once in a compute shader. All subsequent passes read from the expanded buffer (pass-through vertex shader).
4. **Bindless descriptors** — `VK_EXT_descriptor_indexing` with `nonuniformEXT` indexing. One big descriptor set for all textures and material data.
5. **Per-vertex material ID** — each vertex carries a `uint16` that indexes material parameters in a bindless array. Does NOT index shader technique — technique is determined by which shader the material references.
6. **Technique grouping is automatic** — derived from the shader attached to each material. The compute culling stage groups geometry by technique into separate indirect command buffers. One `vkCmdDrawIndexedIndirect` per technique. No manual enum.
7. **Instance data overrides** — per-entity color tint, roughness offset, metalness offset stored in the instance buffer. Reduces need for duplicate material definitions.
8. **ECS authoritative** — entities carry mesh handles and material overrides. CPU writes transforms to a ring-buffered instance array each frame.
9. **Manual GPU memory** — no VMA. A few large persistent buffers (geometry, materials) + ring buffer for per-frame instance data.

---

## 5. Instance Data Layout

One `InstanceData` struct per entity per frame. Written by the CPU, consumed by the compute culling shader.

```cpp
struct InstanceData {
    mat4 model;                    // 64 bytes — from Transform
    uint32_t mesh_offset;          //  4 bytes — offset into combined geometry buffer
    uint16_t material_id;          //  2 bytes — base material (textures + PBR params)
    uint16_t technique_id;         //  2 bytes — derived from Material's shader (automatic)
    vec4  color_tint;              // 16 bytes — multiplied with albedo
    float roughness_offset;        //  4 bytes — added to material roughness
    float metalness_offset;        //  4 bytes — added to material metalness
    // Total: 96 bytes, 3 × 32-byte cache lines
};
```

The shader reads the base material from the bindless array using `material_id`, then applies the per-instance overrides. One material definition produces infinite visual variation.

For radical overrides (different texture), the game creates a new material file and gets a new `material_id`. The instance overrides handle the common case.

---

## 6. Technique Grouping (automatic, shader-derived)

**Not a manual enum.** The system is fully automatic:

1. The game loads a shader → gets a `ShaderHandle`
2. The game creates a `Material` → assigns the `ShaderHandle`
3. The engine assigns a `technique_id` based on the `ShaderHandle` (deduplicated — same shader = same technique)
4. When writing instance data, `technique_id` is included
5. The compute culling shader reads `technique_id` and writes draw commands to the corresponding indirect command buffer
6. The CPU issues one `vkCmdDrawIndexedIndirect` per unique technique

The game never specifies a technique enum. It just assigns shaders to materials, and the engine groups them automatically.

**Technique deduplication:** A `TechniqueManager` (engine) maps `ShaderHandle` → `technique_id`. If two materials reference the same shader, they share a technique. The manager also owns the per-technique `PipelineManager` instances (created with `PipelineConfig` from the material).

---

## 7. Data Flow (Target, per frame)

```
FRAME START
  │
  ├─ CPU: Write InstanceData ring buffer
  │     1. Query ECS: all (Transform + MeshReference + Material)
  │     2. For each entity: pack InstanceData (model, mesh_offset, material_id,
  │        technique_id from Material's shader, color_tint, roughness_offset...)
  │     3. Flush mapped memory
  │
  ├─ COMPUTE PASS 1: Vertex Expansion + Culling (graphics queue)
  │     Input:  raw geometry buffer, instance data ring buffer
  │     Output: expanded position buffer (clip space, for depth)
  │             expanded attribute buffer (world normals, texcoord, material_id)
  │             per-technique indirect command buffers (after frustum culling)
  │
  ├─ DEPTH PRE-PASS (raster, pass-through vertex shader)
  │     Input: expanded position buffer
  │     Indirect draws: one per technique, using commands from compute
  │     Output: depth attachment
  │
  ├─ COMPUTE PASS 2: Occlusion Culling (graphics queue, reads depth)
  │     Input: depth attachment, expanded positions
  │     Output: further reduced per-technique indirect command buffers
  │
  ├─ MAIN PASS (raster, pass-through vertex shader)
  │     For each technique:
  │       Bind technique pipeline
  │       vkCmdDrawIndexedIndirect (commands from occlusion culling)
  │       Vertex: load from expanded attribute buffer
  │       Fragment: material_id → bindless texture array → PBR + instance overrides
  │
  └─ PRESENT
```

### Key insight: the first compute pass is the bottleneck

After that, every raster pass is lightweight:

- Vertex shader is a memory load (pass-through)
- Fragment shader is the only "real" computation per-pixel
- Automatic barriers via render graph between compute and raster

### Expanded vertex buffer layout (compute output)

```cpp
struct ExpandedVertex {
    vec4  clip_position;    // 16 bytes — for depth + culling
    vec3  world_normal;     // 12 bytes — for lighting
    vec2  texcoord;         //  8 bytes
    uint16_t material_id;   //  2 bytes — bindless index
    // Total: 38 bytes → 48 with alignment
};
```

Ring-buffered (2-3 frames) to allow GPU to read while next frame's compute writes.

---

## 8. Key Design Decisions

### 8.1 Per-vertex material ID (not per-meshlet)

| Decision | Rationale |
|---|---|
| Each vertex stores `uint16 material_id` | Avoids meshlet management on GPU. Simplifies culling and indirect draw generation |
| Material ID indexes bindless material params array | Textures, PBR factors (roughness, metalness, albedo) |
| Material ID does NOT index shader technique | Technique is derived from the shader attached to the material, handled automatically |

**Origin of material_id:** Baked during content export (glTF material index, OBJ material group). The engine assigns a global material ID at load time. The app can override per-entity color/roughness/metalness via the `Material` component's instance overrides.

### 8.2 Automatic technique grouping (not enum, not uber-shader)

The engine groups geometry by technique automatically. The technique_id is derived from the shader the material references, not a manual enum.

**Why not uber-shader:** Avoids complex branching and specialization. Technique count stays small (5-20). Each technique gets its own pipeline with optimal state. `VK_EXT_shader_object` is a potential future optimization but not required.

**Why not manual enum:** The game assigns shaders to materials. The engine figures out the rest. Extensible by definition — any new shader creates a new technique automatically.

### 8.3 Compute-first vertex expansion

One compute dispatch transforms all geometry. All raster passes use pass-through vertex shaders.

**Trade-off:** Higher VRAM usage (expanded buffer) vs lower ALU waste (no redundant transforms). Break-even ~500K-1M visible triangles. Design targets above that.

### 8.4 Dedicated compute queue

**Yes, dedicated compute queue.** Rationale:
- Vertex expansion compute pass can overlap with other work
- Culling compute can run while GPU is doing other transfers
- Required for `VK_EXT_device_generated_commands` to reach its potential

The render graph already supports `QueueType::Compute`. Adding queue selection + timeline semaphore synchronization is the implementation work.

### 8.5 Manual GPU memory (no VMA)

Few persistent allocations:
- 1 combined geometry buffer (static, all meshes)
- 1 material data buffer (static, all PBR params)
- 1 bindless texture array (static, all textures)
- 1 expanded vertex buffer (ringed, 2-3 frames)
- 1 instance data ring buffer (double-buffered)
- N indirect command buffers (double-buffered, one per technique)

**RingBuffer model:**
```cpp
class RingBuffer {
    GpuBuffer buffers_[kFrameCount];
    uint64_t write_offset_{0};
public:
    void* Allocate(uint64_t size);   // linear alloc within frame
    void Flip();                     // advance to next buffer
};
```

### 8.6 ResourceSystem scope (CPU only)

| Asset type | CPU-side (ResourceSystem) | GPU-side | Managed by |
|---|---|---|---|
| Mesh data | `MeshResource` (verts, indices, submesh info) | Combined `GpuBuffer` | SceneLoader |
| Texture | `TextureResource` (pixels) | `GpuTexture` in bindless array | ResourceSystem + uploader |
| Shader | `ShaderResource` (SPIR-V) | `vk::ShaderModule` | ShaderLoader + pipeline cache |
| Material def | Material file (PBR params) | Bindless material `GpuBuffer` | MaterialManager |

**ResourceSystem manages CPU-side data only.** GPU upload is explicit, done by SceneLoader and MaterialManager. No automatic "load from disk → upload VRAM" coupling.

### 8.7 Instance data overrides (solves material cloning)

Color tint, roughness, metalness per-instance. The common case of "100 enemies with different colors" uses one material definition + per-instance overrides, not 100 material IDs. For radical overrides (different texture map), the game creates a new material.

---

## 9. Material Definition Format

Loaded from file, not hardcoded. Compatible with glTF PBR material parameters:

```glsl
struct MaterialParams {
    vec4  base_color_factor;           // RGBA
    float metallic_factor;
    float roughness_factor;
    float normal_scale;
    uint16_t albedo_texture_index;      // index into bindless texture array
    uint16_t normal_texture_index;
    uint16_t metallic_roughness_texture_index;
    uint16_t occlusion_texture_index;
    uint16_t shader_handle_id;          // which shader this material uses
    uint16_t padding;
};
```

Loaded from `.mat` files, glTF embedded materials, or a dedicated material format. The `MaterialManager` assigns global IDs and uploads to a GPU-readable buffer.

---

## 10. ECS Role in Rendering

The ECS is authoritative but does not own GPU data. Per-frame flow:

```
ECS UPDATE (CPU):
  ├─ Game modifies Transform, Material on entities
  └─ Done

PRE-RENDER (CPU → GPU):
  ├─ SceneRenderer queries: ForEach (Transform + MeshReference + Material)
  ├─ For each entity, writes InstanceData to ring buffer
  │     model      = Transform::matrix
  │     mesh_offset = MeshReference::offset
  │     material_id = Material::global_id
  │     technique_id = TechniqueManager::GetOrCreate(Material::shader)
  │     color_tint, roughness_offset, metalness_offset = Material::overrides
  └─ Flush ring buffer

GPU FRAME (no CPU involvement):
  ├─ Compute: expand + cull + sort by technique_id → indirect commands
  ├─ Depth pass (raster, pass-through vertex)
  ├─ Compute: occlusion cull → refine indirect commands
  └─ Main pass: one indirect draw per technique, pass-through vertex
```

**Transform is NOT merged into geometry on CPU.** Model matrix is instance data. Compute applies it during vertex expansion. O(n) where n = entity count, not vertex count.

---

## 11. Vulkan Feature Enablement (Target)

| Feature | Extension / Core | Phase | Status |
|---|---|---|---|
| Dynamic rendering | Vulkan 1.3 core | — | ✅ Enabled |
| Synchronization2 | Vulkan 1.3 core | — | ✅ Enabled |
| Descriptor indexing (bindless) | `VK_EXT_descriptor_indexing` | 3 | 🔜 |
| Buffer device address | `VK_KHR_buffer_device_address` | 4 | 🔜 |
| Multi draw indirect | Vulkan 1.1 core | 4 | 🔜 |
| Draw indirect count | `VK_KHR_draw_indirect_count` | 4 | 🔜 |
| Device-generated commands | `VK_EXT_device_generated_commands` | 5 | 🎯 |
| Dedicated compute queue | Device queue discovery | 4 | 🔜 |
| Mesh shaders | `VK_EXT_mesh_shader` | ❌ | Not planned |

---

## 12. Phased Implementation Plan

### Phase 0 — Current foundation ✅

- Camera component
- DefaultRenderer (wraps pipeline + frame lifecycle)
- SceneLoader (mesh/texture load + GPU upload)
- Game class (state + hooks encapsulated)
- MeshRendererSystem with configurable PipelineConfig
- Single mesh, no materials, no culling, no ECS rendering

### Phase 1 — ECS material system + combined geometry buffer

Goal: entities have different meshes and materials. Engine renders them sorted by technique.

**New components:**
- `Material` — material_id, shader handle, texture refs, PBR params, instance overrides
- `MeshReference` — offset + size into combined geometry buffer
- `Renderable` — marker (entity has Transform + MeshReference + Material)

**New modules:**
- `TechniqueManager` — maps shaders → technique_id, owns per-technique pipelines
- `SceneRenderer` — ECS query + instance data packing + draw loop

**Modified:**
- `SceneLoader::UploadToGPU` → combined geometry buffer + per-mesh metadata
- `DefaultRenderer` → holds SceneRenderer, delegates per-frame rendering
- `MeshRendererSystem` → replaced or repurposed

**Modified:**
- `StandardMeshPipeline::Vertex` — add `uint16 material_id` field

**Vertex format:**
```cpp
struct Vertex {
    float px, py, pz;     // position
    float nx, ny, nz;     // normal
    float u, v;           // texcoord
    uint16_t material_id; // material index into bindless array
    // Total: 34 bytes → 36 with padding
};
```

**What the app looks like:**
```cpp
auto mesh_id = scene_loader.GetMeshHandle("helmet");
auto mat_id = material_manager.Load("materials/red_leather.mat");

auto& entity = registry.CreateEntity();
registry.AddComponent<Transform>(entity, pos, rot, scale);
registry.AddComponent<MeshReference>(entity, mesh_id);
auto& mat = registry.AddComponent<Material>(entity, mat_id);
mat.color_tint = glm::vec4(1.0f, 0.5f, 0.5f, 1.0f); // per-instance tint
```

### Phase 2 — Bindless descriptors

- Enable `VK_EXT_descriptor_indexing`
- One giant descriptor set: `sampler2D allTextures[]`, `StructuredBuffer<MaterialParams> allMaterials[]`
- Shaders use `nonuniformEXT(material_id)` for indexing
- `StandardMeshPipeline` descriptor layout updated

### Phase 3 — GPU-driven pipeline (compute + indirect)

- Enable buffer device address, indirect draw features
- Dedicated compute queue
- Compute shader: vertex expansion + frustum culling + technique sorting → indirect commands
- Depth pre-pass (raster, pass-through vertex)
- Compute shader: occlusion culling
- Main pass: one indirect draw per technique
- `DefaultRenderer` / `SceneRenderer` orchestrates the full GPU pipeline

### Phase 4 — Device-generated commands

- `VK_EXT_device_generated_commands`
- Fully GPU-orchestrated rendering
- Fallback: CPU reads culling output and generates indirect commands

---

## 13. Open Questions

**(All resolved — see sections above)**

| Question | Answer |
|---|---|
| 1. Vertex format timing | Add `uint16 material_id` in Phase 1, even though bindless isn't ready — avoids vertex format migration later |
| 2. Technique extensibility | Automatic — derived from shader handle. Any new shader creates a new technique |
| 3. Material definition source | Loaded from file, not hardcoded. Compatible with glTF PBR params |
| 4. Compute queue | Dedicated queue |
| 5. Device-generated commands | Target from the start, fallback to CPU-generated indirect |
| 6. Material ID vs instance overrides | Instance buffer carries color_tint, roughness_offset, metalness_offset. Common variations use overrides, not new materials |
