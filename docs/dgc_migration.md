# DGC (VK_EXT_device_generated_commands) Migration

## Scope

Replace the per-technique CPU loop in `SceneRenderer::Render()` with a single
`vkCmdExecuteGeneratedCommandsEXT` call. The GPU handles pipeline switching
internally via the `EXECUTION_SET` token. All existing compute-driven culling
(expand, occlusion_sort, collect) remains unchanged — only the rendering tier
changes.

The DGC and fallback paths share most of the pipeline (all compute passes,
descriptor setup, depth pass). Branching only happens at the specific points
where they diverge: shader creation, buffer setup, and the main rendering call.

---

## Data Model

### Sequence Record

```cpp
struct SequenceRecord {
    uint32_t executionSetIndex; // selects pipeline from IndirectExecutionSetEXT
    // VkDrawIndirectCommand follows:
    uint32_t vertexCount;
    uint32_t instanceCount;
    uint32_t firstVertex;
    uint32_t firstInstance;
};
// stride = 20 bytes (5 × uint32_t, 4-byte aligned)
```

### Indirect Commands Layout

| Token | Offset | Data |
|---|---|---|
| `EXECUTION_SET` | 0 | `uint32_t executionSetIndex` |
| `DRAW` | 4 | `VkDrawIndirectCommand` |

- `stride = 20`
- `maxSequenceCount` = device limit (capped to an engine upper bound)

### Indirect Execution Set

Created with `initialPipelineCount` matching the DGC layout's
`maxSequenceCount`. Uses the single shared pipeline layout across all
technique pipelines (set 0 = bindless textures, set 1 = submesh vertex blocks,
set 2 = raw vertex buffers, set 3 = compacted indirection).

Slots with no registered technique receive a degenerate pipeline that
discards all primitives. Registered techniques are assigned their pipeline
via `vkUpdateIndirectExecutionSetEXT`.

### Technique Change Notification

`TechniqueManager` holds a `std::function<void(uint16_t id, VkPipeline,
VkPipelineLayout)> on_technique_changed_` callback. `SceneRenderer` sets
this during initialization. When `RegisterTechnique()` is called, the
callback fires and updates the execution set slot:

```cpp
if (dgc_available_) {
    VkWriteIndirectExecutionSetPipelineEXT write{};
    write.executionSet = execution_set_;
    write.index = technique_id;
    write.pipeline = new_pipeline;
    vkUpdateIndirectExecutionSetEXT(device, 1, &write, 0, nullptr);
}
```

### Preprocess Buffer

Allocated per frame-in-flight. Size from
`vkGetGeneratedCommandsMemoryRequirementsEXT` with:

| Field | Value |
|---|---|
| `indirectExecutionSet` | the execution set |
| `indirectCommandsLayout` | the token layout |
| `maxSequenceCount` | same as layout |
| `maxDrawCount` | same as maxSequenceCount |

The preprocess buffer is a DGC scratch buffer, not tracked by the render
graph. Preprocessing and execution happen on the same command buffer in the
main pass; the spec guarantees availability of preprocessed results to the
subsequent execute.

### Per-Frame Buffers

| Buffer | Size | Usage |
|---|---|---|
| `dgc_sequence_buffer` | `maxSequenceCount * 20` | `STORAGE_BUFFER \| INDIRECT_BUFFER \| SHADER_DEVICE_ADDRESS` |
| `dgc_count_buffer` | 4 bytes | same |
| `dgc_preprocess_buffer` | from memory requirements | device-local |

The `maxSequenceCount` is set once at DGC object creation time (from the
device's `maxIndirectSequenceCount`, capped to a sane upper bound like 256).
The sequence buffer is over-allocated to match this max; the collect shader
writes packed sequences at the start of the buffer and only the first N
sequences are processed (via `dgc_count_buffer`). This avoids recreating DGC
objects when technique count changes.

Both `dgc_sequence_buffer` and `dgc_count_buffer` are registered with the
render graph so barriers are correctly emitted between the collect pass
(compute write) and main pass (indirect read). The existing
`technique_draw_commands` buffer (and its counts/offsets array) is removed in
the DGC path but retained in the fallback path.

The `draw_count_buffer` for the depth prepass is unchanged in both paths.

---

## Shader Changes

The original two-pass `collect.comp` is split into three shaders, each with
a single responsibility:

| Shader | Responsibility | Path |
|---|---|---|
| `collect_count.comp` | Pass 0: sum visible index counts per technique | shared |
| `collect_compact.comp` | Pass 1: prefix sum + compact indirection entries | shared |
| `collect_write_dgc.comp` | Read prefix-sum results, write packed DGC sequences | DGC only |
| `collect_write_legacy.comp` | Read prefix-sum results, write technique draw commands | fallback only |

### collect_count.comp (shared)

Unchanged from the current pass 0. Iterates submeshes, accumulates
`tech.counts[t]` for visible entries. Single output: the per-technique count
array.

### collect_compact.comp (shared)

Replaces the current pass 1. Iterates submeshes, builds `s_off[t]` prefix
sum in shared memory, copies visible indirection entries from the full buffer
to the compacted buffer. Writes per-technique offsets to a buffer for
consumption by the write shaders. Same logic regardless of DGC.

Outputs:
- Compacted indirection buffer
- Intermediate buffer: `techStartEntry[t]` (offset in compacted buffer) + `tech.counts[t]` per technique

### collect_write_dgc.comp (DGC path)

Reads the intermediate buffer produced by collect_compact. One workgroup
iterates techniques and writes packed DGC sequences:

```glsl
if (gl_LocalInvocationIndex == 0) {
    uint packedIdx = 0;
    for (uint t = 0; t < pc.techniqueCount; t++) {
        uint cnt = techCounts[t];
        if (cnt == 0) continue;
        uint seq_off = packedIdx * 5u;
        dgc_seq.data[seq_off + 0] = t;                // executionSetIndex = original technique ID
        dgc_seq.data[seq_off + 1] = cnt;               // vertexCount
        dgc_seq.data[seq_off + 2] = 1u;                // instanceCount
        dgc_seq.data[seq_off + 3] = techStartEntry[t]; // firstVertex
        dgc_seq.data[seq_off + 4] = 0u;                // firstInstance
        packedIdx++;
    }
    dgc_cnt.count = packedIdx;
}
```

### collect_write_legacy.comp (fallback path)

Reads the same intermediate buffer. One workgroup writes the old per-technique
`DrawIndirectCommand` array and counts/offsets into `TechBuffer`:

```glsl
if (gl_LocalInvocationIndex == 0) {
    for (uint t = 0; t < pc.techniqueCount; t++) {
        tech.counts[t] = techCounts[t];
        tech.offsets[t] = techStartEntry[t];
        tech.cmds[t] = DrawIndirectCommand(techCounts[t], 1u, techStartEntry[t], 0);
    }
}
```

### Descriptor set layout

The intermediate buffer (techStartEntry + techCounts) uses the same bindings
in both paths. Only the final write shader's output binding differs:

| Binding | Shared | DGC write | Fallback write |
|---|---|---|---|
| 0 | cull blocks (read) | — | — |
| 1 | full indirection (read) | — | — |
| 2 | compacted indirection (write) | — | — |
| 3 | intermediate buffer (read) | DGC sequence buffer (write) | technique draw commands (write) |
| 4 | — | DGC count (write) | — |

### Vertex Shaders

No changes. All use `gl_VertexIndex` to index the compacted indirection
buffer at set 3, which is identical regardless of how the draw was dispatched.

---

## Descriptor Set Layout

### Set 6 (collect count + compact)

Shared across both paths. Three bindings:

| Binding | Resource | Type |
|---|---|---|
| 0 | cull blocks (read) | storage buffer |
| 1 | full indirection buffer (read) | storage buffer |
| 2 | compacted indirection buffer (write) | storage buffer |

The intermediate buffer (techStartEntry + counts) is bound per-frame as a
storage buffer. The count and compact passes share the same set 6 layout.

### Set 7 (collect write)

Two variant layouts, one per path:

**DGC path:**
| Binding | Resource |
|---|---|
| 0 | intermediate buffer (techStartEntry + counts, read) |
| 1 | DGC sequence buffer (write) |
| 2 | DGC count (write) |

**Fallback path:**
| Binding | Resource |
|---|---|
| 0 | intermediate buffer (techStartEntry + counts, read) |
| 1 | technique draw commands buffer (write) |

All other descriptor sets (0–5) are unchanged in both paths.

---

## C++ Changes

### Extension & Feature Enablement

In `VulkanDevice::CreateLogicalDeviceAndResources()`:

1. Query `VkPhysicalDeviceDeviceGeneratedCommandsPropertiesEXT` via
   `vkGetPhysicalDeviceProperties2` on the physical device. Check
   `maxIndirectSequenceCount`, `maxIndirectCommandsTokenCount`, and
   `maxIndirectCommandsIndirectStride` against minimum requirements.
2. If the extension is available and properties satisfy requirements, add
   `VK_EXT_DEVICE_GENERATED_COMMANDS_EXTENSION_NAME` to `device_extensions`
   and add `VkPhysicalDeviceDeviceGeneratedCommandsFeaturesEXT{ .deviceGeneratedCommands = VK_TRUE }`
   to the feature pNext chain.
3. Store a `dgc_available_` runtime boolean on the device/backend.

### SceneRenderer::Initialize()

After common setup (block buffers, descriptor sets 0–5), branch for DGC
vs fallback shader creation and buffer allocation:

```
// ── Shared: set 6 layout (count + compact) ──
//   binding 0: cull blocks
//   binding 1: full indirection
//   binding 2: compacted indirection

// ── Shared: set 7 layout (write shader) ──
//   binding 0: intermediate buffer (techStartEntry + counts)
if (dgc_available_) {
    // set 7 binding 1: DGC sequence buffer (SequenceRecord[])
    // set 7 binding 2: DGC count (uint32_t)
    // load collect_compact.comp (shared)
    // load collect_count.comp  (shared)
    // load collect_write_dgc.comp
} else {
    // set 7 binding 1: technique draw commands (TechBuffer)
    // load collect_compact.comp (shared)
    // load collect_count.comp  (shared)
    // load collect_write_legacy.comp
}
```

DGC-specific object creation:

```
if (dgc_available_) {
    uint32_t maxSeq = std::min(device_max_seq_count, ENGINE_UPPER_BOUND);

    // Create IndirectCommandsLayoutEXT
    //   tokens = [EXECUTION_SET @ 0, DRAW @ 4]
    //   stride = 20, maxSequenceCount = maxSeq

    // Create IndirectExecutionSetEXT
    //   initialPipelineCount = maxSeq
    //   pipelineLayout = shared technique layout
    //   Fill all slots with degenerate pipeline initially

    // Per-frame:
    //   intermediate_buffer   = Create(maxSeq * 8, STORAGE)  // startEntry+count per technique
    //   dgc_sequence_buffer   = Create(maxSeq * 20, STORAGE | INDIRECT | DEVICE_ADDRESS)
    //   dgc_count_buffer      = Create(4, STORAGE | INDIRECT | DEVICE_ADDRESS)
    //   dgc_preprocess_size   = GetGeneratedCommandsMemoryRequirements(...)
    //   dgc_preprocess_buffer = Create(dgc_preprocess_size, device-local)
}

// Fallback: intermediate_buffer same size, technique_draw_commands same as today
```

Set up the technique callback:

```
if (dgc_available_) {
    technique_manager.SetTechniqueCallback(
        [this](uint16_t id, VkPipeline pipeline, VkPipelineLayout) {
            VkWriteIndirectExecutionSetPipelineEXT write{ .executionSet = execution_set_,
                .index = id, .pipeline = pipeline };
            vkUpdateIndirectExecutionSetEXT(device, 1, &write, 0, nullptr);
        });
}
```
if (dgc_available_) {
    uint32_t maxSeq = std::min(device_max_seq_count, ENGINE_UPPER_BOUND);

    // Create IndirectCommandsLayoutEXT
    //   tokens = [EXECUTION_SET @ 0, DRAW @ 4]
    //   stride = 20, maxSequenceCount = maxSeq

    // Create IndirectExecutionSetEXT
    //   initialPipelineCount = maxSeq
    //   pipelineLayout = shared technique layout
    //   Fill all slots with degenerate pipeline initially

    // Per-frame:
    //   dgc_sequence_buffer = Create(maxSeq * 20, STORAGE | INDIRECT | DEVICE_ADDRESS)
    //   dgc_count_buffer    = Create(4, STORAGE | INDIRECT | DEVICE_ADDRESS)
    //   dgc_preprocess_size = GetGeneratedCommandsMemoryRequirements(...)
    //   dgc_preprocess_buffer = Create(dgc_preprocess_size, device-local)
}
```

Fallback path: allocate and set up the old `technique_draw_commands` buffer
unchanged.

Set up the technique callback:

```
if (dgc_available_) {
    technique_manager.SetTechniqueCallback(
        [this](uint16_t id, VkPipeline pipeline, VkPipelineLayout) {
            VkWriteIndirectExecutionSetPipelineEXT write{ .executionSet = execution_set_,
                .index = id, .pipeline = pipeline };
            vkUpdateIndirectExecutionSetEXT(device, 1, &write, 0, nullptr);
        });
}
```

### SceneRenderer::Render() (main pass)

Branch only at the rendering call:

```
// Common: bind the 4 shared descriptor sets
cmd.bindDescriptorSets(PIPELINE_BIND_POINT, shared_layout, 0,
    { bindless_set, submesh_vertex_set, raw_vertex_set, indirection_set });

if (dgc_available_) {
    // VkGeneratedCommandsInfoEXT:
    //   shaderStages = VERTEX
    //   indirectExecutionSet = execution_set
    //   indirectCommandsLayout = layout
    //   indirectAddress = device_address of dgc_sequence_buffer
    //   indirectAddressSize = maxSeq * 20
    //   preprocessAddress = device_address of dgc_preprocess_buffer
    //   preprocessSize = dgc_preprocess_size
    //   maxSequenceCount = maxSeq
    //   sequenceCountBuffer = device_address of dgc_count_buffer
    //   sequenceCountOffset = 0
    //   sequenceCount = 0

    vkCmdPreprocessGeneratedCommandsEXT(cmd, &genInfo);
    vkCmdExecuteGeneratedCommandsEXT(cmd, &genInfo);
} else {
    for (uint16_t t = 0; t < technique_count; ++t) {
        cmd.bindPipeline(technique[t].pipeline);
        // descriptor sets already bound above
        cmd.drawIndirect(technique_draw_commands, offset_of(t), 1, sizeof(VkDrawIndirectCommand));
    }
}
```

Both paths share: viewport/scissor setup, descriptor set binding, and the
push constant for view-projection. Only the draw dispatch differs.

### DefaultRenderer Integration (Render Graph)

The render graph registers the DGC buffers (or the legacy buffer) depending
on the active path:

```
auto draw_indirect = pipeline_->ImportBuffer("draw-indirect");
auto dgc_count     = pipeline_->ImportBuffer("dgc-count");
```

The three collect dispatches (count, compact, write) all write into buffers
consumed by the main pass. The render graph tracks:
- Intermediate buffer: written by collect_compact, read by collect_write
- Sequence buffer or technique draw commands: written by collect_write, read by main pass (IndirectDraw)
- DGC count buffer: written by collect_write_dgc, read by main pass (IndirectDraw)

Existing scene buffers and depth/hiz attachments are unchanged.

---

## On-GPU Execution Order

```
expand.comp    → writes full indirection + cull entries
depth pass     → vkCmdDrawIndirect (unchanged)
hiz_gen        → generates Hi-Z mip chain
occlusion_sort → zeros indexCount for occluded submeshes

// Three compute dispatches:
collect_count.comp   → pass 0: sum visible index counts per technique
collect_compact.comp → pass 1: prefix sum + compact indirection entries

if dgc_available:
    collect_write_dgc.comp   → write packed SequenceRecord[] + count
else:
    collect_write_legacy.comp → write TechBuffer (DrawIndirectCommand[])

[render graph barrier]

// Main pass (common descriptor binding):
cmd.bindDescriptorSets(4 shared sets)

if dgc_available:
    vkCmdPreprocessGeneratedCommandsEXT(...)
    vkCmdExecuteGeneratedCommandsEXT(...)
      │  seq[0]: bindPipeline(execSet[ID=7]),  draw(visible)
      │  seq[1]: bindPipeline(execSet[ID=12]), draw(visible)
      │  seq[2]: bindPipeline(execSet[ID=3]),  draw(visible)
      │  ...packedIdx sequences
else:
    for each technique:
        bindPipeline + drawIndirect
```

---

## Pipeline Compatibility

All technique pipelines must share:
- Same `VkPipelineLayout` (same descriptor set layouts at same bindings)
- Same vertex input state (zero bindings for SSBO pulling)
- Same dynamic rendering info (single color + depth format)
- Same dynamic states (viewport + scissor)

The depth prepass pipeline is not part of the execution set — different
layout, standalone indirect draw.

---

## GPU Address Requirements

DGC uses `VkDeviceAddress` for sequence buffer, count buffer, and preprocess
buffer. `bufferDeviceAddress` is already enabled. All three buffers must
include `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT`.

---

## Fallback Path

When `dgc_available_` is false:
- DGC objects are not created
- `collect_count.comp` and `collect_compact.comp` are loaded (same as DGC)
- `collect_write_legacy.comp` is loaded instead of `collect_write_dgc.comp`
- The old `technique_draw_commands` buffer (TechBuffer) is allocated and
  bound at set 7 binding 1
- `Render()` uses the existing per-technique loop
- Branching is at shader selection, buffer allocation, and the draw dispatch

---

## Resource Lifecycle

| Object | Owner | Lifetime |
|---|---|---|---|
| `VkIndirectCommandsLayoutEXT` | SceneRenderer | entire renderer lifetime |
| `VkIndirectExecutionSetEXT` | SceneRenderer | entire renderer lifetime |
| Intermediate buffers (×3) | SceneRenderer::FrameResources | per-frame ring |
| Preprocess buffers (×3) | SceneRenderer::FrameResources | per-frame ring (DGC only) |
| DGC sequence buffers (×3) | FrameResources | per-frame ring (DGC only) |
| DGC count buffers (×3) | FrameResources | per-frame ring (DGC only) |
| technique_draw_commands (×3) | FrameResources | per-frame ring (fallback only) |

All destroyed during `SceneRenderer::Shutdown()` after `device.waitIdle()`.

---

## Queries & Validation

At startup, query `VkPhysicalDeviceDeviceGeneratedCommandsPropertiesEXT`:

| Property | Check |
|---|---|
| `maxIndirectSequenceCount` | Must be ≥ 1 (capped to engine upper bound) |
| `maxIndirectCommandsTokenCount` | Must be ≥ 2 (EXECUTION_SET + DRAW) |
| `maxIndirectCommandsIndirectStride` | Must be ≥ 20 |
| `minIndirectCommandsBufferOffsetAlignment` | Sequence buffer alignment |
| `minIndirectSequenceCountBufferOffsetAlignment` | Count buffer offset alignment |

If any minimum is unsatisfied or the extension is absent, DGC is disabled.
