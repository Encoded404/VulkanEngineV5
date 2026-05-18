# Phase 1 Implementation — ECS Material System + Combined Geometry Buffer

## Overview

Phase 1 transforms the renderer from a single-mesh, single-pipeline prototype into a proper ECS-driven renderer supporting multiple meshes, materials, and techniques. Every entity can have a different mesh and material, and the engine automatically groups and sorts draws for efficiency.

## Key Design Decisions

### 1. Vertex format includes `uint16 material_id` (even though bindless isn't ready)

**Why now:** Changing the vertex format later means rewriting loaders, shaders, and pipeline creation. Adding the field now avoids a painful migration.

**Layout (36 bytes per vertex):**
```
float px, py, pz;       // offset 0  — position
float nx, ny, nz;       // offset 12 — normal
float u, v;             // offset 24 — texcoord
uint16_t material_id;   // offset 32 — material index (for Phase 2+)
uint16_t _padding;      // offset 34 — alignment to 4 bytes
```

### 2. Combined geometry buffer

All meshes are uploaded into one big vertex buffer + one big index buffer. Per-mesh metadata (vertex offset, index offset, index count) is tracked by the SceneLoader and queried by the MeshReference component.

### 3. Per-entity material grouping (not per-vertex material indexing)

In Phase 1, the material_id vertex attribute exists but is unused. Material selection is done per-entity via the Material component. The renderer groups entities by (technique_id, descriptor_set) to minimize state changes.

### 4. Technique is derived from the shader

A TechniqueManager maps shader (vert + frag SPIR-V) to a technique_id. If two materials use the same shader, they share a technique. The TechniqueManager owns the per-technique PipelineManager instances.

### 5. MaterialManager is programmatic

No file loading yet. Materials are registered at init with shader + texture + PBR params. The MaterialManager assigns a global material_id and manages the descriptor set for each material. File-based material loading (.mat files) is deferred to a later phase.

### 6. SceneRenderer replaces MeshRendererSystem

The new SceneRenderer:
1. Queries all entities with (Transform + MeshReference + Material)
2. Groups by technique_id
3. Within each technique, groups by descriptor_set (material)
4. Binds pipeline once per technique, descriptor set once per material, push constants per entity
5. Draws using the entity's mesh offset + index count from MeshReference

### 7. Push constants remain the MVP (64 bytes)

In Phase 1, each entity still gets its own push constant with the model-view-projection matrix. This is computed CPU-side from the Transform component and Camera matrices.

## New Modules

| Module | Files | Purpose |
|---|---|---|
| `VulkanEngine.Components.Material` | `Components/Material.cppm` | Stores material_id, technique_id, instance overrides |
| `VulkanEngine.Components.MeshReference` | `Components/MeshReference.cppm` | Vertex/index offset into combined buffer |
| `VulkanEngine.TechniqueManager` | `TechniqueManager/TechniqueManager.*` | Maps shader→technique_id, owns pipelines |
| `VulkanEngine.MaterialManager` | `MaterialManager/MaterialManager.*` | Registers materials, manages descriptor sets |
| `VulkanEngine.SceneRenderer` | `SceneRenderer/SceneRenderer.*` | ECS query, sort, draw |

## Modified Modules

| Module | Change |
|---|---|
| `StandardMeshPipeline::Vertex` | Add `uint16_t material_id` + padding |
| `StandardMeshPipeline::CreatePipeline` | Add 4th vertex attribute (R16_UINT for material_id) |
| `SceneLoader` | Combined buffer upload, return per-mesh metadata |
| `DefaultRenderer` | Holds SceneRenderer, delegates per-frame rendering |
| `MeshRenderer` component | Deprecated — kept for backward compat but not used by SceneRenderer |
| `app/Game` | Uses new components + managers |
| `engine/CMakeLists.txt` | Add new source/module files |

## Data Flow (per frame, Phase 1)

```
DefaultRenderer::RenderFrame()
  ├── InternalRenderPass() calls SceneRenderer::Render()
  │     ├── Query ECS: entities with (Transform + MeshReference + Material)
  │     ├── Sort by technique_id, then by descriptor_set
  │     ├── For each technique group:
  │     │     ├── Bind technique pipeline
  │     │     ├── For each material (descriptor set) within technique:
  │     │     │     ├── Bind descriptor set
  │     │     │     ├── For each entity in material group:
  │     │     │     │     ├── BuildPushConstants(transform, view, proj)
  │     │     │     │     ├── pushConstants(MVP)
  │     │     │     │     └── drawIndexed(offset = mesh_ref.vertex_offset)
  │     │     │     └── End material group
  │     │     └── End technique group
  │     └── End sorted draw
  ├── ImGui overlay
  └── Present barrier
```
