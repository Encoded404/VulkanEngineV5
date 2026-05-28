# Submesh Material Refactor

## Problem

Every vertex has a `material_id` field that is meant to be a bindless texture
array slot index. In practice, every model gets `material_id = 0` because:

1. File loaders read `material_index` per-submesh from model files, but the
   loading pipeline (`LoadedMeshData`) drops all submesh info.
2. `ConvertToVertices` assigns one uniform `material_id` to every vertex of a
   mesh (from `Game.cpp`'s `active_slot`).
3. `Game.cpp` passes the same texture slot for every mesh.

There is no system to map file-local material IDs (0, 1, 2...) to engine-global
technique + texture slot combinations.

## Solution Overview

- Delete the `Material` component entirely.
- `SubMesh` stores resolved `technique_id` + `texture_slot` (no raw
  `material_index`).
- `MaterialManager` stores `MaterialDefinition { technique_id, texture_slot }`.
  Mappings at load time convert file material indices to MaterialManager IDs.
- One entity = one mesh (not one per submesh). `MeshReference` stores
  `first_submesh` + `submesh_count`.
- `PrepareCompute` expands each entity into N `CompactSubmesh` entries (one per
  submesh).
- Split compact submesh data into dynamic (position+rotation, changes every
  frame) and static (index range + technique + texture slot, rarely changes).
- `BlockBuffer` class: shared block-based container for all per-submesh buffers
  (dynamic, static, bounding spheres, SubmeshVertexData, etc.). No hard limit on
  submesh count.
- Per-vertex `material_id` removed. Texture slot comes from
  `SubmeshVertexData[submeshId].texture_slot` in the vertex shader.
- New `collect.comp` compute shader post-occlusion: compacts visible submeshes
  sorted by technique, generates per-technique draw commands.

---

## 1. Handle System

Strongly-typed handles prevent mixing up `uint16_t` values. Each lives in a
small `.cppm` alongside its manager module.

```
engine/TechniqueManager/TechniqueId.cppm
  struct TechniqueId { explicit TechniqueId(uint16_t v) : value(v) {} uint16_t value; };

engine/BindlessManager/TextureSlot.cppm
  struct TextureSlot { explicit TextureSlot(uint16_t v) : value(v) {} uint16_t value; };

engine/MaterialManager/MaterialId.cppm
  struct MaterialId { explicit MaterialId(uint16_t v) : value(v) {} uint16_t value; };
```

Files that only need to pass a handle around can import just the tiny handle
module instead of the full manager module.

---

## 2. Material System

### MaterialDefinition (MaterialManager.cppm)

Stripped down to just what the GPU needs:

```cpp
struct MaterialDefinition {
    TechniqueId technique_id{0};
    TextureSlot texture_slot{0};
};
```

No more `base_color_factor`, `metallic_factor`, `roughness_factor` — those
would come from textures if PBR is added later.

### MaterialManager: singleton

```cpp
class MaterialManager {
public:
    static MaterialManager& Get();
    static void Initialize(backend, technique_manager, bindless_manager);
    static void Shutdown();

    MaterialId Register(const MaterialDefinition& def);
    const MaterialDefinition& Get(MaterialId id) const;
    uint16_t GetCount() const;
};
```

Called once at startup. No need to pass references around.

### Fallback material

Registered at startup as `MaterialId{0}`:
- `TechniqueId{0}` = default unlit shader
- `TextureSlot{0}` = missing texture (checkerboard)

### Material component: DELETED

No more `engine/Components/Material.cppm`. The technique + texture slot live on
each `SubMesh` instead.

---

## 3. SubMesh Architecture

### SubMesh struct (MeshTypes.cppm)

```cpp
struct SubMesh {
    uint32_t index_start{0};
    uint32_t index_count{0};
    TechniqueId technique_id{0};
    TextureSlot texture_slot{0};
};
```

`material_index` is removed. Values are **resolved** at load time — the file
loaders still read raw material indices from disk, but a mapping step converts
`file_material_index → MaterialId → technique_id + texture_slot` before the
data enters the engine pipeline.

### One entity = one mesh, expanded into N submeshes

Entities are logical game objects (a box, a character, a door). A mesh may have
multiple submeshes (e.g. box edges use one material, box core uses another).

`MeshReference` stores the range into a global submesh array:

```cpp
class MeshReference : public Component {
public:
    uint32_t first_submesh = 0;   // index into SceneRenderer's submesh array
    uint32_t submesh_count = 0;   // how many submeshes this mesh spans
    uint8_t  index_buffer_index = 0;
};
```

`PrepareCompute` expands each entity:

```
for each entity with MeshReference:
  for s = 0..entity->submesh_count:
    submesh = scene_submeshes_[entity->first_submesh + s]
    write CompactSubmeshDynamic[count]  (position, rotation from Transform)
    write CompactSubmeshStatic[count]   (index_start, index_range, technique_id, texture_slot from submesh)
    write BoundingSphere[count]         (center + radius)
    count++
```

Rationale for NOT making entities per-submesh:
- A box with 2 materials would need 2 entities → physics, ownership, and
  gameplay code becomes convoluted.
- The entity is a gameplay concept; submeshes are a rendering concern.

---

## 4. Buffer Architecture

### CompactSubmesh: split into dynamic + static

**Dynamic** (host-visible, rewritten every frame):
```
vec3  position;       // 12B  from Transform
float pad;            //  4B  align next to 16B
vec4  rotation;       // 16B  quaternion from Transform
// Total: 32B per entry
```

**Static** (host-visible, written once or on material change):
```
uint32_t index_start;    //  4B  packed: (indexBufIdx << 24) | localOffset
uint32_t index_range;    //  4B
uint16_t technique_id;   //  2B
uint16_t texture_slot;   //  2B
uint32_t pad;            //  4B  align to 16B
// Total: 16B per entry
```

### All per-submesh buffers

| Buffer | Per entry | Memory | Written by | Read by |
|---|---|---|---|---|
| CompactSubmeshDynamic | 32B | HOST_VISIBLE | CPU (PrepareCompute) | expand.comp |
| CompactSubmeshStatic | 16B | HOST_VISIBLE | CPU (once at load, rarely update) | expand.comp |
| BoundingSphere | 16B | HOST_VISIBLE | CPU (PrepareCompute) | occlusion cull |
| SubmeshVertexData | 80B | DEVICE_LOCAL | expand.comp (GPU) | vertex shader, occlusion cull |
| SubmeshCullBuffer | 16B | DEVICE_LOCAL | expand.comp then occlusion (GPU) | collect.comp |

### BlockBuffer class (new)

Shared block container replacing fixed-size MAX_ENTITIES arrays.

```
engine/GpuResources/BlockBuffer.cppm + .cpp
```

```cpp
class BlockBuffer {
public:
    struct Config {
        uint32_t entry_size = 0;
        uint32_t entries_per_block = 256;
        vk::BufferUsageFlags extra_usage = {};
        vk::MemoryPropertyFlags memory = HOST_VISIBLE | HOST_COHERENT;
    };

    bool Initialize(IVulkanBootstrap& backend, const Config& cfg);

    // Ensures at least 'count' entries exist. Returns pointer to entry 0
    // (for host-visible blocks). nullptr for device-local blocks.
    void* EnsureCapacity(uint32_t count);

    // Access individual entry (only valid after EnsureCapacity)
    void* Get(uint32_t index);

    // Descriptor binding helpers
    uint32_t BlockCount() const;
    vk::Buffer GetBlockBuffer(uint32_t block_index) const;
    uint64_t BlockSize() const;          // bytes per block
    uint32_t EntriesPerBlock() const;    // entries per block

private:
    void AddBlock();
    Config cfg_;
    std::vector<GpuBuffer> blocks_;
    std::vector<void*> mappings_;  // non-null only for host-visible
};
```

Used by all five per-submesh buffers in `FrameResources`:

```cpp
struct FrameResources {
    BlockBuffer compact_dynamic;     // 32B per entry
    BlockBuffer compact_static;      // 16B per entry
    BlockBuffer bounding_spheres;    // 16B per entry
    BlockBuffer submesh_vertex_data; // 80B per entry (device-local, GPU-writable)
    BlockBuffer submesh_cull;        // 16B per entry (device-local, GPU-writable)
    // ...
};
```

### Why blocks (not a single resizable buffer)

1. **No hard cap.** Add blocks as needed, never recreate a monolithic buffer.
2. **No reallocation cost.** New blocks = new allocation + update descriptor
   array. Existing data stays put.
3. **Descriptor array** with `PARTIALLY_BOUND` flag — only bind blocks that
   exist. 1024 blocks × 256 entries = 262K submesmes max.

### Indirection buffers (not block-based)

The full and compacted indirection buffers are sized by total index count, not
submesh count. They stay as single `GpuBuffer` instances sized from the loaded
data, same as today.

---

## 5. Loading Pipeline

### File loaders

Loaders (OBJ, Bin, glTF) parse files as before. They read raw `material_index`
from the file into `SubMesh.material_index` (kept temporarily during loading,
removed after resolution).

### Resolution step (after loading, before upload)

Developer supplies a mapping in `Game.cpp`:

```cpp
MaterialId mat_a = material_mgr.Register({technique = pbr_tech,   texture_slot = metal_slot});
MaterialId mat_b = material_mgr.Register({technique = unlit_tech, texture_slot = emissive_slot});

std::vector<Mesh> meshes = SceneManager::LoadMeshes(path);
for (auto& mesh : meshes) {
    for (auto& sm : mesh.subMeshes) {
        // file_material_map[sm.material_index] resolves file-level material
        // index "0" to an engine MaterialId, then we extract technique+slot.
        auto& def = material_mgr.GetMaterial(file_material_map[sm.material_index]);
        sm.technique_id = def.technique_id;
        sm.texture_slot = def.texture_slot;
    }
}
```

After resolution, `material_index` is consumed and no longer needed.

### LoadedMeshData / CombinedScene

`LoadedMeshData` gains no submesh info (it stays flat). Instead, the mesh's
`SubMesh[]` array is carried separately through `UploadCombined`.

```cpp
struct MeshInfo {
    std::string name;
    uint32_t vertex_offset, vertex_count;
    uint32_t index_offset, index_count;
    uint32_t first_submesh_index;   // into combined submeshes[]
    uint32_t submesh_count;
};

struct CombinedScene {
    HeapAllocation vertex_allocation;
    HeapAllocation index_allocation;
    std::vector<MeshInfo> meshes;
    std::vector<SubMesh> submeshes;  // flattened, offsets adjusted to combined buffer
};
```

`UploadCombined` concatenates vertices and indices as before, but also carries
submeshes through and adjusts their `index_start` values by the mesh's global
index offset.

### Vertex struct

```cpp
struct Vertex {
    float px, py, pz;    // 12B
    float nx, ny, nz;    // 12B
    float u, v;          //  8B  — total: 32B (8 uints)
};
```

`material_id` and `padding` removed. `ConvertToVertices` no longer takes a
`default_material_id` parameter.

---

## 6. GPU Pipeline (Frame Flow)

### CPU-side changes: sorting removed

The current `PrepareCompute` sorts entities by `material->technique_id` and
builds `TechniqueDrawRange` / `current_ranges_` for the CPU render loop. After
the refactor, **no CPU-side sorting or range-building happens**. Sorting by
technique is done entirely on the GPU by `collect.comp`.

`DrawEntity` is simplified — the `Material*` pointer is removed since technique
comes from `SubMesh`:
```cpp
struct DrawEntity {
    const Transform* transform = nullptr;
    const MeshReference* mesh = nullptr;
};
```

The render loop no longer iterates `current_ranges_`. Instead it iterates
GPU-generated draw commands.

### Full frame flow

```
Frame N:

  CPU: PrepareCompute
    ├─ Gather entities with MeshReference + Transform components
    ├─ For each entity, iterate its submeshes (from scene_submeshes_[]):
    │     write CompactSubmeshDynamic[count]  (position, rotation)
    │     write CompactSubmeshStatic[count]   (index_start, range, tech, texture)
    │     write BoundingSphere[count]         (center, radius)
    │     count++
    ├─ BlockBuffer::EnsureCapacity(count)
    ├─ Upload dynamic + sphere data → GPU
    └─ Dispatch expand.comp with count

  GPU: expand.comp (1 thread per submesh)
    ├─ Read: CompactSubmeshDynamic + CompactSubmeshStatic
    ├─ Compute MVP from position + rotation + viewProj
    ├─ Write SubmeshVertexData[submeshId] = { MVP, texture_slot }
    ├─ Write SubmeshCullBuffer[submeshId] = { indirection_offset, index_count, technique_id }
    ├─ atomicAlloc in indirection_buffer, write { vertexId, submeshId } pairs
    └─ (indirection buffer = all submeshes, unsorted)

  GPU: Depth Prepass
    ├─ Bind full indirection_buffer (set 3)
    └─ vkCmdDrawIndirect (single draw, all depth)

  GPU: Hi-Z Gen

  GPU: Occlusion Cull
    ├─ Read: SubmeshVertexData (MVP), BoundingSpheres, Hi-Z
    ├─ For occluded submeshes: SubmeshCullBuffer[id].index_count = 0
    └─ (does NOT touch indirection_buffer)

  GPU: collect.comp (2 dispatches)
    ├─ Dispatch 1: Count visible indices per technique
    │     for each submesh where index_count > 0:
    │       atomicAdd(tech_total[T], index_count)
    ├─ Dispatch 2: Compact + generate draw commands
    │     compute prefix sums → per-technique output offsets
    │     for each visible submesh:
    │       copy indirection entries → compacted_buffer at tech offset
    │     write per-technique VkDrawIndirectCommand
    └─ Output: compacted_indirection_buffer (technique-sorted) + technique_draw_cmds

  GPU: Main Pass
    ├─ For each technique with visible submeshes:
    │     bind technique's pipeline
    │     bind compacted_indirection_buffer (set 3)
    │     vkCmdDrawIndirect(technique_draw_cmds[T])
    └─ One draw call per technique
```

### Descriptor sets & layout changes

Block-based buffers use bindless storage buffer arrays (`descriptorCount =
MAX_BLOCKS` with `PARTIALLY_BOUND | UPDATE_AFTER_BIND`), same pattern as the
existing bindless vertex/index arrays. This means the descriptor set layouts
for sets 1, 4, and 5 change from single-buffer bindings to array bindings.

| Set | Bound to | Contents |
|---|---|---|
| 0 | All graphics | Bindless texture array (BindlessManager, unchanged) |
| 1 | Vertex shader, occlusion | SubmeshVertexData blocks (`storage buffer[]`) |
| 2 | Vertex shader | Bindless vertex buffer array (unchanged) |
| 3 | Vertex shader | Full indirection buffer (depth pass) or compacted (main pass) |
| 4 | expand.comp | CompactSubmeshDynamic blocks, CompactSubmeshStatic blocks, SubmeshVertexData blocks (write), SubmeshCullBuffer blocks (write), indirection buffer (write), draw count (write), index buffer array (read) |
| 5 | occlusion cull | SubmeshVertexData blocks, SubmeshCullBuffer blocks (read/write), BoundingSpheres blocks, Hi-Z |
| 6 | collect.comp | SubmeshCullBuffer blocks (read), full indirection buffer (read), compacted indirection buffer (write), technique draw commands buffer (write) |

The draw count buffer from expand persists for the depth prepass. The
technique draw commands buffer is new — an array of `VkDrawIndirectCommand`
written by collect.comp and consumed by the main pass.

---

## 7. Shader Changes

### Vertex shader (main_world.vert)

- `rawBase` changes from `vertexID * 9u` to `vertexID * 8u` (no more
  material_id in vertex data).
- Read texture_slot from `SubmeshVertexData` (set 1, binding 0):
  ```glsl
  outMaterialId = submeshVertexData[nonuniformEXT(submeshId)].texture_slot;
  ```
- `outMaterialId` stays as `flat out uint` — the fragment shader interface
  doesn't change.

### Depth vertex shader (depth_world.vert)

- Only change: `rawBase = vertexID * 8u`.

### Fragment shader (standard_mesh.frag)

- **No interface changes.** Still receives `flat in uint inMaterialId` and uses
  `allTextures[nonuniformEXT(inMaterialId)]`.

### expand.comp

- Reads CompactSubmeshDynamic (position, rotation) + CompactSubmeshStatic
  (index_start, index_range, technique_id, texture_slot).
- Writes SubmeshVertexData blocks (MVP + texture_slot).
- Writes SubmeshCullBuffer blocks (indirection_offset, index_count,
  technique_id).
- Writes indirection buffer entries as `IndirectionEntry { uint vertexId; uint
  submeshId; }` (struct instead of raw uint pairs).

### occlusion_sort.comp

- **No longer reads or writes the indirection buffer** — no index expansion.
- Reads SubmeshVertexData blocks for MVP (projects bounding sphere).
- Reads SubmeshCullBuffer (reads existing `index_count`, writes 0 if culled).
- Reads BoundingSpheres blocks + Hi-Z for the occlusion test.
- **No longer needs the bindless index buffer array** (was needed only for
  index expansion, which is gone from this shader).
- Push constants: no longer needs `viewProj` (MVP comes from
  SubmeshVertexData). Only needs `submeshCount` + viewport dimensions.

### collect.comp (new, separate descriptor set 6)

2-dispatch compute shader:
1. **Dispatch 1** (count): Iterate SubmeshCullBuffer. For each entry where
   `index_count > 0`, `atomicAdd(tech_total[technique_id], index_count)`.
2. **Dispatch 2** (compact + draw commands): Compute prefix sums from
   `tech_total[]` into `tech_offset[]` (shared memory). For each visible
   submesh, atomically copy its indirection entries from the full buffer to the
   compacted buffer at the per-technique offset. Write per-technique
   `VkDrawIndirectCommand { vertexCount = tech_total[T], firstVertex =
   tech_offset[T], instanceCount = 1 }`.

Set 6 bindings: SubmeshCullBuffer blocks (read), full indirection buffer
(read), compacted indirection buffer (write), technique draw commands array
(write).

### IndirectionEntry struct

The indirection buffer entries change from manual `uint` pair indexing to a
clear struct:

```glsl
struct IndirectionEntry {
    uint vertexId;    // packed: (vertBufIdx << 24) | localVertexID
    uint submeshId;   // index into SubmeshVertexData for MVP + texture_slot
};
```

---

## 8. Entity & Component Changes

### Removed: Material component

The files `engine/Components/Material.cppm` (and `.cpp` if it exists) are
deleted. All imports of `VulkanEngine.Components.Material` are removed.

### Modified: MeshReference component

```cpp
class MeshReference : public Component {
public:
    uint32_t first_submesh = 0;
    uint32_t submesh_count = 0;
    uint8_t  index_buffer_index = 0;
};
```

### Unchanged: Camera, SimpleControllerComponent, TransformControlComponent

No changes needed for Camera. SimpleControllerComponent and TransformControlComponent are app-level components.

### SceneRenderer stores submeshes

```cpp
// SceneRenderer private member:
std::vector<SubMesh> scene_submeshes_;

// New method called after UploadCombined:
void SetSubmeshes(std::vector<SubMesh> submeshes);
```

`PrepareCompute` reads from this array via `MeshReference.first_submesh` +
`submesh_count`.

---

## 9. Files

### New files

| File | Contents |
|---|---|
| `engine/TechniqueManager/TechniqueId.cppm` | Handle struct |
| `engine/BindlessManager/TextureSlot.cppm` | Handle struct |
| `engine/MaterialManager/MaterialId.cppm` | Handle struct |
| `engine/GpuResources/BlockBuffer.cppm` | BlockBuffer class definition |
| `engine/GpuResources/BlockBuffer.cpp` | BlockBuffer implementation |
| `engine/shaders/collect.comp` | Post-occlusion compaction shader |

### Modified files

| File | Changes |
|---|---|
| `engine/MaterialManager/MaterialManager.cppm` | Simplify MaterialDefinition, make static |
| `engine/MaterialManager/MaterialManager.cpp` | Static singleton impl |
| `engine/Mesh/MeshTypes.cppm` | SubMesh: replace material_index with technique_id + texture_slot |
| `engine/StandardMeshPipeline/StandardMeshPipeline.cppm` | Vertex: remove material_id + padding |
| `engine/SceneLoader/SceneLoader.cppm` | CombinedScene adds submeshes, MeshInfo adds submesh range |
| `engine/SceneLoader/SceneLoader.cpp` | UploadCombined carries + adjusts submeshes, ConvertToVertices no material_id |
| `engine/Components/MeshReference.cppm` | first_submesh + submesh_count |
| `engine/SceneRenderer/SceneRenderer.cppm` | BlockBuffer FrameResources, SetSubmeshes, remove MAX_ENTITIES, remove TechniqueDrawRange/current_ranges_, new descriptor layouts (bindless arrays for blocks), add compacted indirection + technique draw commands buffers |
| `engine/SceneRenderer/SceneRenderer.cpp` | BlockBuffer init, PrepareCompute submesh expansion (no sorting, no DrawEntity::Material*), remove technique_id_buffer_, descriptor layout creation for new sets, depth/main pass use different indirection buffers, Render() iterates GPU draw commands |
| `engine/DefaultRenderer/DefaultRenderer.cpp` | Add collect.comp dispatch, no Material refs |
| `engine/shaders/expand.comp` | Read dynamic+static blocks, write SubmeshVertexData blocks + SubmeshCullBuffer blocks, IndirectionEntry struct |
| `engine/shaders/main_world.vert` | rawBase=8u, texture_slot from SubmeshVertexData blocks via nonuniformEXT |
| `engine/shaders/depth_world.vert` | rawBase=8u |
| `engine/shaders/occlusion_sort.comp` | Read SubmeshCullBuffer, mark culled, no indirection rewrite, no index buffer binding needed, simplified push constants |
| `app/Game.cpp` | MaterialManager singleton, remove Material component, mapping array |
| `app/Game.cppm` | Remove Material import |

### Deleted files

| File |
|---|
| `engine/Components/Material.cppm` |
| `engine/Components/Material.cpp` (if exists) |

---

## 10. Open Questions (for future)

- **collect.comp implementation details:** 1 dispatch or 2? Shared memory prefix
  sums vs global atomics. Can be decided when writing the shader.
- **Static data update path:** When a material changes at runtime (e.g. texture
  swap), the static block entry needs updating. The BlockBuffer mapping allows
  direct memcpy — a simple `UpdateStaticEntry(index, new_data)` method suffices.
- **SubmeshCullBuffer in collect.comp:** The cull buffer needs to be read by
  collect.comp. Since both are GPU-written and GPU-read, no CPU sync is needed.
  Just a pipeline barrier between occlusion and collect.

---

