# Skeletal animation and vertex modifiers

The engine's animation path: skeletons, skins, animation clips, a per-frame joint
palette, compute pre-skinning, and an engine-owned vertex-modifier stack. Skinning
is the first modifier; the same buffers, dispatch list and pass are what an
application modifier uses.

The standard mesh vertex and fragment shaders need no skinning code. Deformation
writes a per-frame vertex buffer, and the vertex puller reads it through
`VertexEntry::deformBias` exactly as it reads ordinary geometry. No technique,
`Vertex` format or vertex-input state changes.

## 1. Asset model

`engine/core/assets/MeshTypes.cppm` owns the types:

- `BoneWeight` — CPU/import type: four `uint16_t` joint indices + four `uint8_t`
  weights summing to 255.
- `Skeleton` — `joints` (each with a parent index or `kNoSkeleton`, and its
  bind-pose local TRS), `roots` (a joint plus the world transform of the
  non-joint ancestor above it), and display `names`.
- `Skin` — which `Skeleton` it belongs to, skin-local joint ordinal →
  skeleton joint index, and one inverse-bind matrix per joint (column-major).
- `AnimationClip` / `AnimationChannel` / `AnimationKey` — sampled TRS keys.
- `Mesh::bone_weights`, `Mesh::bone_weights_extra`, `Mesh::influence_sets`
  (0/1/2), `Mesh::skeletons`, `Mesh::skins`, `Mesh::animations`, and
  `SubMesh::skin_index` (`kNoSkin` when unskinned).

Influences are **4 + optional 4** (`kMaxInfluenceSets = 2`, eight total). Joint
indices are stored **skin-local**: `Skin::joints[k]` names the skeleton joint that
skin joint `k` drives, so one vertex stream can serve several skins. More than
eight influences, or a streamed/dynamic mesh, is not supported (§7).

The GPU-facing record packs the three words so no 16-bit storage-buffer feature is
needed (12 B):

```cpp
struct GpuBoneInfluence {
    uint32_t joints01; // j0 | (j1 << 16)
    uint32_t joints23; // j2 | (j3 << 16)
    uint32_t weights;  // w0 | (w1 << 8) | (w2 << 16) | (w3 << 24)
};
```

## 2. glTF import

`engine/core/assets/FileLoaders/Mesh/GltfMeshAssembler.cpp` imports
`JOINTS_0`/`WEIGHTS_0` and optionally `JOINTS_1`/`WEIGHTS_1`:

- Joint accessors may be `u8` or `u16`; weight accessors may be `f32` or
  normalized `u8`/`u16`. `fastgltf::iterateAccessor` resolves the component type,
  normalization and sparsity, so the assembler reads widened values.
- Weights are normalized **across both sets** and quantized to `u8` summing to
  exactly 255. Each set contributes up to four influences, so a vertex has at
  most eight.
- The vertex dedup key includes all eight joint indices and weights, so two
  corners that share a position but belong to different joints are not merged.
- Skeletons are deduped by their ordered joint-node list: two skins over the same
  joints share one `Skeleton`, keeping one `SkeletonPose` sufficient for both.
- A joint with no joint parent becomes a `SkeletonRoot`; its transform is the
  accumulated world transform of the non-joint ancestors (the armature node), so
  a non-identity armature offsets the mesh consistently with its inverse-bind
  matrices.
- A skinned node's own transform is ignored, as glTF requires: its submesh
  placement stays identity and the object transform is applied through the entity.
- Geometry is emitted once per glTF mesh and shared across node placements,
  including placements with different skins; each placement gets its own
  `SubMesh::skin_index`.
- `CUBICSPLINE` samplers are decoded by taking the middle output value per key and
  storing it as `Linear` (reported once per clip). `Linear` rotation is
  shortest-arc slerp.
- Animation channels target scene nodes. The importer maps each channel to the
  owning skeleton's joint index; non-joint targets are dropped with a warning, and
  one animation targeting several skeletons is split into
  `<name>#<skeleton-index>` clips.
- Missing `TANGENT` is generated with MikkTSpace from the authored UVs.

`BinSkinnedMeshAssembler` remains only as a compatibility shim for the BIN loader:
BIN has weights but no hierarchy, inverse-bind matrices or clips, so it cannot
drive animation and is not wired into the renderer.

## 3. Runtime components

`MeshData` carries the same skeletal data into
`MeshRegistry::LoadedMeshInfo::cpu_data`. Registry storage is not stable (it is
replaced on re-registration and removed on eviction), so components never alias
it:

```cpp
auto asset = VulkanEngine::MakeSkeletalAsset(mesh_data); // self-contained copy
```

`SkeletalAsset` owns copies of the skeletons, skins and clips; `SkeletonPose` and
`AnimationPlayer` share it through a `shared_ptr<const SkeletalAsset>`.

### `SkeletonPose`

`engine/core/components/SkeletonPose.cppm` holds the per-object animation state:

- `local` — one `SubMeshTransform` per joint (TRS).
- `world` — composed model-space matrices.
- `palettes` — one matrix array per **skin**; only skins whose `Skin::skeleton`
  equals `skeleton_index` are populated. `GetPalette(skin)` returns an empty span
  for every other skin, so a multi-skeleton asset can never read another
  skeleton's matrices.
- `version` — bumped on every rebuild; gather copies the current palette each
  frame.

`Rebuild()` fills `local` from each joint's bind pose and then calls
`RebuildFromLocal()`. `RebuildFromLocal()` composes the world matrices
order-independently (children are found from parent indices, not array order) and
computes `palette[k] = world[skin.joints[k]] * inverse_bind[k]`. The stored
palette is column-major; the deform shader multiplies with
`mul(float4(pos, 1), m)` (the engine's row-vector convention).

An app may drive the pose by hand exactly as the `skinned_scene` example does:
restore the bind-pose rotations, write `local`, call `RebuildFromLocal()`, all
before gather runs.

### `AnimationPlayer` and `AnimationSystem`

`engine/core/components/AnimationPlayer.cppm` adds clip playback:
`clip_index`, `time`, `speed`, `loop`, `playing`, plus the same `asset`. The pair
is valid only when both components share the **same asset pointer** and
`asset->animations[clip_index].skeleton == pose.skeleton_index`; a mismatch is
reported once and ignored rather than driving another skeleton's matrices.

`AnimationSystem::Update` (registered in `FrameSlot::AnimationUpdate`, before
`FrameSlot::MeshProcessFrame`) advances and wraps or clamps time, calls
`SampleClip`, rebuilds the pose, and bumps `version`. `SampleClip` first fills
every joint from the bind pose and then overwrites only the clip's channels, so
switching to a clip with fewer joints restores the missing joints to bind pose.
Rotation channels use shortest-arc slerp: both endpoints are normalized, the
second is negated on a negative dot product, and the result is unit length.
A pose with no player is left alone once it is settled; only `dirty` or an
asset/skeleton change triggers a bind-pose rebuild.

## 4. Per-frame GPU path

```
gather (CPU)
  → expand (GPU: transforms, addressing, deform allocation)
  → deform (GPU: skinning / modifier stack)
  → occluder-select
  → occluder prepass → Hi-Z → pre-cull → depth prepass → Hi-Z → occlusion
  → collect → main pass
```

`deform` is a built-in pass (`BuiltinPass::Deform`), registered between `expand`
and `occluder-select` in `Renderer.cpp`. It is not an app pass: it needs the
engine's private per-frame buffers, and an async-compute pass may not touch them.

### Gather

`MeshGatherSystem` resolves each submesh and writes one `SkinEntry` plus, for
skinned submeshes, a palette:

- **Skin resolution** requires `SubMesh::skin_index != kNoSkin`, a `SkeletonPose`
  on the entity, `skin.skeleton == pose.skeleton_index`, and a non-empty palette.
  Otherwise the submesh renders through the normal vertex path (bind pose).
- **Palette allocation** is a per-frame bump allocator over the mapped flat
  `bone_palettes` buffer, cached by `(entity, skin_index)` so submeshes sharing a
  skin share one palette. `palette_base` is a **global matrix index**; it is never
  clamped, because a clamped base aliases another skin's bones. On exhaustion the
  submesh's deform is disabled and the shortfall is reported once.
- **Modifier stack**: prepend the builtin skinning stage when runnable, then the
  object stack and (for `Append`) the submesh stack. Dead ids, parameter-size
  mismatches and pipelines that are not ready are dropped **before** stage
  numbers are assigned.
- **Dispatch list**: one CPU-side `ModifierDispatch` per runnable stage, in order.
  It is never uploaded.
- **Bounds**: every active deform gets `kDeformNoOccluder | kDeformNoCullTest`;
  `expand` copies them into the cull entry. See §6.

### Per-frame records

`SceneRenderer.cppm` exports the C++ mirrors; `shaders/skin_layout.slang` mirrors
the shader side, pinned by `static_assert`s.

`SkinEntry` (32 B, one per submesh, block array in the expand and deform sets):

| Field | Meaning |
|---|---|
| `palette_base` | global matrix index into the flat palette buffer |
| `skin_slot`, `skin_bias` | skin-influence table slot and index bias |
| `extra_slot`, `extra_bias` | second influence set, `kNoSkinSlot` when absent |
| `influence_sets` | 0/1/2 |
| `flags` | `kDeformActive`, `kDeformNoOccluder`, `kDeformNoCullTest`, `kSkinned`, `kFinalRegionB` |
| `deform_base` | written by **expand**, read by the deform pass |

`ModifierPC` (32 B) is pushed per dispatch: `submesh_index`, `stage`, `src_slot`,
`src_base`, `vertex_count`, `param_offset`, `param_size`, `region_span`.

`ModifierDispatch` is the CPU-side counterpart: which modifier, which stage,
the source table slot/absolute base, the vertex count, and the parameter block.

### Expand

`expand.slang` owns the per-frame deform-region allocator — the third word of
`expandCounter`:

- If `hasActiveDeform == 0` the shader **never reads the skin-entry array** and
  writes `deformBias = kNoDeform`. This is what keeps an unskinned scene free of
  extra descriptor reads.
- Otherwise, if the entry has `kDeformActive`, `InterlockedAdd(expandCounter[2],
  vertexSpan, deformBase)` reserves `[deformBase, deformBase + vertexSpan)`. If it
  does not fit in the frame's `regionSpan`, `kDeformActive` is cleared and
  `deformBias` is `kNoDeform`: the submesh renders its bind pose instead of
  corrupting another submesh's region.
- On success the final region is `deformBase + (kFinalRegionB ? regionSpan : 0)`
  and `VertexEntry.deformBias = finalBase - (baseVertex + windowBase)`.
- `kDeformNoOccluder`/`kDeformNoCullTest` are copied into `CullEntry.flags`.

The push constant carries `hasActiveDeform` and `regionSpan` (`T`, one region per
submesh — the buffer is `2 * T` vertices).

### Deform

`deform.slang` is the builtin `"skinning"` modifier. Its pipeline layout is
`[vertex_buffers_layout_, deform_layout_]`:

- set 0: the existing vertex table (both bindings were widened to
  `eVertex | eCompute`; the UV binding is unused by compute).
- set 1: skin table, extra skin table, flat palette `StructuredBuffer<float4x4>`,
  skin-entry block array, `RWStructuredBuffer<Vertex> deformVertices`, and the
  flat `ByteAddressBuffer modifierParams`.

Stage addressing is element indices:

```
A = entry.deform_base
B = entry.deform_base + pc.region_span
dst(stage) = (stage & 1) ? B : A
src(stage) = stage == 0 ? pc.src_base : ((stage & 1) ? A : B)
```

Two regions suffice for any 1:1 stack: every stage reads its source region and
writes its destination at the same element index. A stage must never read the
destination region it is writing.

Each vertex is either rigid (all weights zero — passed through unchanged) or the
weighted sum of `palette`-transformed positions, normals and tangents; the result
is packed back with `PackTBN`. Transform of normals/tangents by the blended joint
matrices and renormalization is the standard cheap approximation of the exact
cofactor transform.

`DispatchDeform` binds the two sets, iterates the frame's CPU dispatch list,
pushes `ModifierPC`, dispatches `ceil(vertex_count / workgroup_size)` groups, and
inserts `COMPUTE_SHADER → COMPUTE_SHADER` barriers between dispatches.

### Vertex shaders

`main_indir.slang` and `depth_indir.slang` read `VertexEntry` before the vertex
fetch and use:

```slang
Vertex vert = (info.deformBias != kNoDeform)
    ? deformVertices[info.deformBias + (int)vertexIndex]
    : vertexBuffers[NonUniformResourceIndex(vertexBufferSlot)][vertexIndex];
```

So the depth prepass and main pass consume deformed vertices from the standard
pipelines. Deform output is object-space; the entity transform is still applied
by the existing MVP path. `OccluderPrePass`, `DepthPrePass` and `MainPass` all
declare a read of the `deform-vertices` graph resource; without it the graph does
not order the deform dispatch before those draws.

## 5. Application vertex modifiers

Any registered compute kernel can sit on the stack. A modifier is strictly 1:1 in
vertices and writes the same element indices it reads.

### Registration

```cpp
// CPU-only; no pipeline is built here.
Modifiers::ModifierDesc desc{};
desc.name = "wave";
desc.compute_shader = shader_id;      // from ShaderManager
desc.param_size = sizeof(WaveParams);
desc.param_alignment = 16;
desc.workgroup_size = 64;
Modifiers::ModifierId id = engine.RegisterModifier(desc);

// Later, when the effect is gone:
engine.UnregisterModifier(id);
```

`ModifierManager` validates a valid compute-shader id, `param_size <= 4096`, a
power-of-two `param_alignment` in `[4, 256]`, and `workgroup_size` in `[1, 1024]`.
The builtin `"skinning"` entry is registered by the engine before the renderer is
created and cannot be unregistered. Every mutation bumps `RegistryVersion()`;
`SyncModifierPipelines` runs in `FrameSlot::ModifierSync` (before gather) and
reconciles the registry into compute pipelines. A pipeline stays alive for
`frames_in_flight` frames after retirement because `PipelineSlot` destroys its
Vulkan pipeline immediately.

### Stack resolution

`GeometryModifier` (`engine/core/components/GeometryModifier.cppm`) is an
object-level data component:

```cpp
Components::GeometryModifier gm;
gm.modifiers = {...};                      // object stack
gm.submeshes.resize(2);
gm.submeshes[1] = Components::SubMeshModifierStack{
    .mode = Components::ModifierStackMode::Replace,
    .modifiers = {...},
};
```

Effective order for a submesh:

```
[skinning?] + object stack + (Append ? submesh stack : nothing)
```

- `nullopt` and `Append{}` both inherit the object stack.
- `Replace{}` runs only the submesh entries; an empty `Replace{}` disables
  modifiers for that submesh.
- The skinning stage is prepended by gather and cannot be removed or reordered.
- Submesh indices are **object-local**, like `MaterialOverride`; `mesh_id` adopts
  lazily and a mismatch is reported once. Reordering a mesh's submeshes
  invalidates the mapping.
- A dropped stage (dead id, wrong parameter size, pipeline not ready) never leaves
  a hole: stages are numbered after the drops.
- A `GeometryModifier` on a streamed/dynamic mesh is ignored with a warning.

### Parameter blocks

Each instance's `params` must be exactly `ModifierDesc::param_size` bytes. Gather
bump-allocates at `AlignUp(cursor, max(param_alignment, 16))` in the frame's flat
`modifier_params` buffer and records `param_offset`/`param_size` in the dispatch.
The buffer starts at `kInitialModifierParamBytes` (1 MiB) and grows geometrically.
If the mapped span is too small, app modifiers are dropped for that frame while
skinning continues, and the affected plans are re-resolved.

### Shader contract

An app modifier includes `engine/core/shaders/modifier_common.slang`, declares
`[numthreads(workgroup_size, 1, 1)]` matching its `ModifierDesc`, and may declare
only the bindings it uses (the engine layout always has all six). It must:

1. Early-out with `if (j >= modifierPC.vertex_count) return;` before any read or
   write. The dispatch rounds up to whole workgroups; without the guard the tail
   writes into the next submesh's region.
2. Read stage 0 from `sourceVertices[modifierPC.src_slot][modifierPC.src_base + j]`,
   or later stages from `deformVertices[ModifierSrc(...) + j]`.
3. Write only `[dst, dst + modifierPC.vertex_count)`.
4. **Never read the destination region it is writing.** Sampling the source region
   at other indices is allowed; reading the in-flight destination races the
   dispatch.
5. Preserve UVs, and write `PackTBN` if positions, normals or tangents change.
6. Read parameters with `modifierParams.Load<T>(modifierPC.param_offset + k)`;
   offsets are 4-byte aligned.
7. Not add, remove or reorder vertices.

`engine/core/shaders/modifier_template.slang` is the checked-in example; the GPU
tests compile a wrapper around it. The `deformVertices` binding is not exposed to
app render passes: the built-in deform pass is its only writer and the standard
vertex shaders its only readers.

## 6. Bounds policy

A deformed object no longer fits its rest-pose bounds. Gather therefore sets
**both** `kDeformNoOccluder` and `kDeformNoCullTest` for every active deform,
skinned or not:

- `occluder_select.slang` skips it, so it is never used as an occluder.
- `pre_cull.slang` and `occlusion_cull.slang` keep it alive without testing its
  stale bounds.

`GeometryModifier::bounds` defaults to `SkipCulling`, the only implemented policy.
`RestPose`, `Conservative` and `Manual` cannot be honoured until a post-deform
envelope exists; they are reported once per object and remain effectively
`SkipCulling`. A later conservative-bounds feature may narrow this, but no code
may leave an active deform with both bits clear.

## 7. Limitations

- **Eight influences**, as two 4-influence sets (`JOINTS_0`/`WEIGHTS_0` and
  `JOINTS_1`/`WEIGHTS_1`). There is no arbitrary-count vertex format; a vertex
  weighted by more than eight bones is not representable.
- **One skeleton per object.** Use child entities for a second skeleton;
  `SkeletonPoseSet` is not implemented.
- **Streamed / `DynamicMesh` geometry is not skinned.** It logs once and falls
  back to the bind pose; the persistent skin heaps are per-`MeshData`.
- **BIN meshes are unskinned.** BIN carries no hierarchy, inverse-bind matrices or
  clips.
- **Cubic-spline clips are played as linear** (middle value only), not as true
  Hermite curves.
- **No compression or streaming.** Clips and skeletons stay uncompressed in
  memory.
- **No morph targets, cloth, destruction or GPU-generated topology.** The stack
  interface leaves room for them; only skinning and 1:1 app modifiers ship.
- **One glTF file is one `Mesh`.** Multi-mesh assets are not merged.
- **64 skin-influence block slots** (`kMaxSkinBuffers`, mirroring the vertex
  table). Exhaustion is reported once; a scene with extremely many small skin
  heaps can hit it.

## 8. Capacity and descriptor budget

- `deform_vertex_span` (`T`) is the sum of the active submeshes' vertex windows;
  the per-frame deformed-vertex buffer holds `2 * T` vertices. The palette buffer
  is flat and sized by `palette_matrix_count`; the parameter buffer by
  `modifier_param_bytes`. All three grow together in
  `SceneRenderer::EnsureSceneCapacity`, which re-creates every frame-in-flight
  slot's buffer in one device idle. Gather calls it **before** mapping any buffer.
- The expand set is `5` block arrays + `3` single storage buffers; `expandCounter`
  is three `uint` words.
- The deform set is `1` block array + `131` single storage buffers (64 skin + 64
  extra skin + palette + output + parameters).
- `bone_palettes` is a flat `GpuBuffer`, not a `BlockArray`: a palette may start
  mid-block or cross a block boundary.
- Skin heap allocations are 48-byte aligned (`LCM(12, 16)`), so
  `skin_region_base = offset / 12` is exact.
- The two skin tables are `ePartiallyBound | eUpdateAfterBind` and are included in
  `EngineBootstrap`'s update-after-bind budget.

## 9. Diagnostics

- Skinned submeshes are validated only through the ordinary mesh path; there is no
  separate skin uploader. Import warnings (invalid skin, missing inverse-bind
  matrices, dropped influences, non-joint animation targets) are emitted once per
  offending object.
- A skinned submesh with no `SkeletonPose`, a mismatched `Skin::skeleton`, or an
  empty palette simply follows the unskinned vertex path (bind pose) rather than
  reading a wrong palette.
- GPU pass timing (`--gpu-profile`) reports the deform dispatch as the `deform`
  pass row, so its cost is visible per frame. A scene with no active deform
  records no dispatch and `HasActiveDeform()` stays false.
  On `examples/skinned_scene` (80 vertices, two joints) on llvmpipe the row is
  about 0.018 ms mean / 0.053 ms max, versus 0.147 ms mean for `expand`; that
  number is only a sanity check. On a real GPU the pass scales with the deformed
  vertex count and the number of stack stages, not with scene size — an unskinned
  submesh records no dispatch at all.
- Tests: `skeletal_animation_tests`, `gltf_skin_import_tests`,
  `gltf_animation_import_tests`, `scene_import_skin_tests`,
  `deform_addressing_tests`, `animation_system_tests`, `modifier_*_tests` are
  device-free. `skinning_gpu_tests`, `modifier_gpu_tests`,
  `unskinned_regression_gpu_tests`, `normal_pack_gpu_tests` and
  `skinned_bounds_gpu_tests` carry the `gpu` label.

## 10. Example

`examples/skinned_scene` is the minimal end-to-end example: it loads the
two-bone `2BoneRectangle.glb`, creates a `SkeletonPose`, and rotates both bones
with a sine wave before gather each frame. See its `models/README.md` for the
asset's provenance and weighting. For clip playback, add an `AnimationPlayer`
sharing the same `SkeletalAsset` and point it at a clip index.
