# Phase 1 Implementation Summary

## Files Created

| File | Purpose |
|---|---|
| `engine/Components/Material.cppm` | ECS component: material_id, technique_id, instance overrides (color_tint, roughness_offset, metalness_offset) |
| `engine/Components/MeshReference.cppm` | ECS component: vertex_offset, index_offset, index_count into combined geometry buffer |
| `engine/TechniqueManager/TechniqueManager.cppm/.cpp` | Maps shaders to technique IDs, owns per-technique PipelineManagers |
| `engine/MaterialManager/MaterialManager.cppm/.cpp` | Registers materials, allocates descriptor sets from technique pipelines, manages material definitions |
| `engine/SceneRenderer/SceneRenderer.cppm/.cpp` | ECS query → sort by technique+material → bind pipeline → bind descriptor set → push constants → drawIndexed |
| `docs/phase1_implementation.md` | Phase 1 design decisions |
| `docs/renderer_architecture_plan.md` | Full long-term architecture plan (updated) |

## Files Modified

| File | Change |
|---|---|
| `engine/StandardMeshPipeline/StandardMeshPipeline.cppm` | Added `uint16_t material_id` + `uint16_t _padding` to Vertex. Total 36 bytes. |
| `engine/StandardMeshPipeline/StandardMeshPipeline.cpp` | Added 4th vertex attribute (location 3, R16_UINT) for material_id |
| `engine/SceneLoader/SceneLoader.cppm/.cpp` | Added `CombinedScene` struct, `LoadAllMeshes()`, `UploadCombined()`. Removed old static singleton pattern. |
| `engine/DefaultRenderer/DefaultRenderer.cppm/.cpp` | Replaced single PipelineManager + MeshRendererSystem with TechniqueManager/MaterialManager + SceneRenderer |
| `engine/CMakeLists.txt` | Added 5 new module files + 3 new source files |
| `app/Game.cppm/.cpp` | Uses TechniqueManager, MaterialManager, SceneLoader::CombinedScene, SceneRenderer |
| `tests/app/CMakeLists.txt` | Updated for new Game module |

## Files Deprecated (kept for now)

| File | Reason |
|---|---|
| `engine/MeshRendererSystem/MeshRendererSystem.cppm/.cpp` | No longer used by DefaultRenderer. Kept for reference. Will be removed in Phase 2. |
| `engine/Components/MeshRenderer.cppm` | No longer used. Replaced by MeshReference + Material. |

## Architecture After Phase 1

### Per-frame render flow:
```
DefaultRenderer::InternalRenderPass()
  └── SceneRenderer::Render()
        ├── ForEach<Material>: collect all entities with (Material + Transform + MeshReference)
        ├── Sort by (technique_id, material_id)
        ├── Set viewport, scissor, bind combined vertex/index buffers
        ├── For each technique group:
        │     ├── Bind pipeline from TechniqueManager
        │     ├── For each material group within technique:
        │     │     ├── Bind descriptor set from MaterialManager
        │     │     ├── For each entity:
        │     │     │     ├── Build MVP from Transform
        │     │     │     ├── pushConstants(MVP)
        │     │     │     └── drawIndexed(offset from MeshReference)
        │     │     └── End material group
        │     └── End technique group
        └── End
```

### Game setup flow:
```
Game::OnSetup()
  ├── Load shader SPIRV
  ├── Create TechniqueManager → RegisterTechnique(vert, frag, PipelineConfig) → technique_id
  ├── Create MaterialManager → RegisterMaterial({technique_id, texture}) → material_id
  ├── Load meshes from disk → CombinedScene (combined vertex/index buffer + per-mesh metadata)
  ├── Create DefaultRenderer
  ├── For each mesh: create entity with Transform + MeshReference(offset) + Material(material_id, technique_id)
  ├── Create camera entity
  └── Done
```

## Design Decisions and Trade-offs

### 1. Combined geometry buffer
- **Decision:** One vertex buffer + one index buffer for all meshes
- **Why:** Required for GPU culling later (compute shader reads all geometry from one buffer). Avoids per-mesh buffer management.
- **Trade-off:** Can't add/remove individual meshes without re-uploading the entire buffer. Acceptable for game scenes where geometry is loaded once.

### 2. Per-vertex material_id in vertex format now
- **Decision:** Added `uint16_t material_id` to Vertex struct during Phase 1
- **Why:** Changing vertex format later means rewriting loaders, pipelines, and shaders. Adding now avoids a migration.
- **Trade-off:** 4 extra bytes per vertex (with padding). 3M verts = 12MB extra VRAM. Negligible.

### 3. TechniqueManager as separate module
- **Decision:** Techniques are a separate concept from materials
- **Why:** Multiple materials can share the same shader (technique). The engine deduplicates pipelines automatically.
- **Trade-off:** One more manager to wire up at init. Worth the separation of concerns.

### 4. MaterialManager doesn't own the descriptor pool
- **Decision:** Materials allocate descriptor sets from the TechniqueManager's per-pipeline pools
- **Why:** The descriptor set layout is defined by the pipeline. Each technique has its own layout.
- **Trade-off:** Materials from different techniques can't share descriptor pools. Fine for Phase 1.

### 5. SceneRenderer queries ForEach<Material>, not a JOIN
- **Decision:** Iterate Material components, get Transform + MeshReference from owner entity
- **Why:** The ECS doesn't support multi-type JOIN queries
- **Trade-off:** O(n) iteration where n = material count. Fine for Phase 1. Can optimize later with archetype-based ECS queries.

### 6. No file-based material loading yet
- **Decision:** Materials are registered programmatically in Game::OnSetup
- **Why:** The material file format isn't designed yet. File loading can be added later without API changes.
- **Trade-off:** Each material requires C++ code to define. Fine for the demo phase.

## Next Steps

Phase 1 provides the foundation. The engine now:
- ✅ Supports multiple meshes per scene
- ✅ Supports multiple materials per scene
- ✅ Groups draws by technique/material for efficiency
- ✅ Prepares vertex format for bindless (material_id)
- ✅ Maintains the render graph + dynamic rendering pipeline

**Phase 2 (bindless descriptors)** would:
- Enable `VK_EXT_descriptor_indexing`
- Replace per-pipeline descriptor sets with one giant bindless set
- Use `nonuniformEXT(material_id)` in shaders
- Eliminate per-material descriptor set binding

**Phase 3 (GPU-driven)** would:
- Enable buffer device address, indirect draw
- Replace CPU sorting with compute shader culling
- Implement compute-first vertex expansion
- Reduce CPU draw overhead to near zero
