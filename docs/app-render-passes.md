# Application render passes

The render graph is the single dispatch path for every GPU pass, built-in or
application-provided. Applications add work by registering an
`IPipelinePass` with the engine; the engine constructs the pass context, calls
`Setup()`, validates the declarations, allocates descriptor sets and pipelines,
orders the pass in the frame, and calls `Execute()`.

The worked example is `examples/custom_pass/`.

## Registering a pass

Get the pipeline after `InitRenderer` and register a pass. Registration returns
a stable `PassHandle` (used for ordering and enable/disable) or a structured
`PassError`:

```cpp
auto& pipeline = engine_game_.GetRenderPipeline();

auto handle = pipeline.RegisterPass(std::make_unique<MyPass>(vert_id, frag_id));
if (!handle.has_value()) {
    // handle.error().code / .message / .pass
}
```

`RegisterPass` is engine-managed: it creates the `PassSetupContext` with the
current render extent, calls `MyPass::Setup(ctx)`, validates every declared
resource and descriptor binding, and tracks the pass. Changes are applied at
the next `ApplyChanges()` (the top of the frame), never mid-frame.

## Setup declarations

`Setup()` declares everything the engine needs. Nothing is created by the pass
by hand.

```cpp
void MyPass::Setup(PassSetupContext& ctx) {
    // Graph resources
    const auto backbuffer = ctx.ReadBackbuffer();
    const auto scene_color = ctx.CreateTransientImage(TransientImageDesc{
        .name = "my-scene-color",
        .format = vk::Format::eR8G8B8A8Unorm,
        .width_scale = 1.0f,   // relative to the render extent
        .height_scale = 1.0f,
        .usage = vk::ImageUsageFlagBits::eColorAttachment |
                 vk::ImageUsageFlagBits::eSampled,
    });
    ctx.AddRead(backbuffer, PipelineStageIntent::FragmentShader, AccessIntent::Read);
    ctx.AddWrite(scene_color);

    // Attachments
    PassAttachmentSetup attachments{};
    attachments.auto_begin_rendering = true;
    AttachmentInfo color{};
    color.resource = scene_color;
    color.load_op = vk::AttachmentLoadOp::eClear;
    color.store_op = vk::AttachmentStoreOp::eStore;
    attachments.color_attachments.push_back(color);
    ctx.SetPassAttachments(attachments);

    // Engine-owned pipeline (formats are inferred from the attachments)
    ctx.RequestGraphicsPipeline(vert_id_, frag_id_);
    // or: ctx.RequestComputePipeline(compute_id_);

    // Engine-owned descriptors (app sets start at 5)
    DescriptorDecl texture{};
    texture.set = 5;
    texture.binding = 0;
    texture.kind = DescriptorKind::SampledImage;
    texture.descriptor_type = vk::DescriptorType::eCombinedImageSampler;
    texture.stage_flags = vk::ShaderStageFlagBits::eFragment;
    ctx.DeclareBindings({texture});
    ctx.BindResource(5, 0, backbuffer);   // descriptor written from the resolved resource
}
```

Resource kinds: `ReadBackbuffer()`, `ReadDepthBuffer()`, `ImportImage(name)`,
`ImportBuffer(name)`, `CreateTransientImage(desc)`, `CreateTransientBuffer(desc)`.
Transients are shared by name: a second pass can call `CreateTransientImage`
with the same name/description to obtain the same handle.

### Relative transient sizes

`TransientImageDesc::width_scale`/`height_scale` (when > 0) size the image from
the current render extent. The engine reallocates size-dependent transients on
resize without recompiling the plan.

## Execute and engine-owned objects

`Execute()` receives a fully populated `FrameContext`. The engine has already
allocated the pipeline, layout, and descriptor sets, and rewired the
descriptors from this frame's resolved resources:

```cpp
void MyPass::Execute(const FrameContext& ctx, vk::CommandBuffer cmd) {
    if (ctx.pass_pipeline == nullptr) return;
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, ctx.pass_pipeline);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, ctx.pipeline_layout,
                           ctx.first_app_descriptor_set, ctx.app_descriptor_sets, {});
    cmd.draw(3, 1, 0, 0);
}
```

Useful `FrameContext` fields: `render_extent`, `render_width`/`render_height`,
`frame_index`, `ring_index`, `swapchain_image_index`, `view`/`proj`,
`techniques`, `bindless`, `registry`, `imgui`, `resource_lookup`,
`pipeline_layout`, `pass_pipeline`, `app_descriptor_sets`,
`first_app_descriptor_set`, `default_sampler`.

Descriptor sets are per frames-in-flight slot and rewritten from the resources
resolved that frame, so a reallocated transient is picked up automatically and
an in-flight frame is never updated.

## Ordering

Passes are ordered by declared resource hazards; explicit ordering uses
built-in anchors:

```cpp
ctx.RunAfter(BuiltinPass::MainPass);   // must run after the main pass
ctx.RunBefore(BuiltinPass::ImGui);     // must run before the ImGui overlay
```

Or with handles:

```cpp
pipeline.AddDependency(first_handle, second_handle); // first before second
```

Explicit dependencies win over inferred write-after-read/write-after-write
ordering, so `RunBefore(BuiltinPass::ImGui)` keeps an app pass ahead of the
overlay even though the overlay is registered earlier.

## Shaders and hot reload

Register shaders through the engine path (generated modules) and watch their
directory:

```cpp
auto& shader_mgr = engine_game_.GetContext().GetShaderManager();
const auto vert = Shaders::MyApp::Vert::Register(shader_mgr, config.shader_data_dir);
engine_game_.AddShaderDirectory(config.shader_data_dir);
```

## Multi-queue (async compute)

A pass can request the dedicated compute queue:

```cpp
void ExposurePass::Setup(PassSetupContext& ctx) {
    ctx.RequestComputePipeline(compute_shader_);
    ctx.SetQueueType(RenderGraph::QueueType::Compute);
}
```

Requirements and behaviour:

- `RenderPipeline::IsAsyncComputeAvailable()` reports whether the device exposes
  a compute-capable queue family distinct from graphics. Registration of a
  compute-queue pass fails with `ValidationFailed` when it does not, so the pass
  must fall back to graphics (the example passes `IsAsyncComputeAvailable()` into
  its compute pass and only then calls `SetQueueType`).
- A compute-queue pass is recorded into a command buffer on the compute queue,
  so registration additionally rejects (`ValidationFailed` or
  `InvalidDeclaration`):
  - declaring render attachments (dynamic rendering is graphics-only),
  - requesting a graphics pipeline, and
  - reading, writing, or binding an imported (engine-owned) resource — those are
    exclusive to the graphics family. Use transients.
- The engine partitions the ordered passes into **queue runs** (maximal
  contiguous same-queue spans), records each run into its own command buffer, and
  submits them in order. A run on a different queue than the previous one is
  separated by a binary semaphore; same-queue runs are ordered by submission. A
  graph may produce at most `RenderGraph::kMaxQueueRuns` runs; more is a
  compile-time error, not a truncated plan.
- The engine's per-frame scene prep (uploads, descriptor writes, Hi-Z
  initialization, optional physical-camera compositing) is graphics work. It is
  folded into the first graphics run when the graph starts on graphics, and
  otherwise recorded into a dedicated graphics preamble run submitted ahead of
  the graph, so a graph may start with a compute pass.
- Graph-declared transients are created with concurrent sharing when more than
  one queue family exists, so no queue-family ownership transfers are needed.
- Each run is a fresh command buffer, so a graphics pass must set its own
  dynamic viewport/scissor rather than relying on a previous pass.
- Barrier scopes are clamped to each pass's queue at plan time, so a compute run
  never names a graphics-only stage or access. Cross-queue ordering is carried by
  the run-boundary semaphore; layout transitions are preserved.
- GPU statistics are collected per queue run (one query per run, in that run's
  own command buffer) and summed for the frame log, so an async frame reports its
  compute passes. Compute runs use a compute-only statistics pool, because a
  pipeline-statistics pool that enables graphics counters may only be used from
  a graphics command pool.
- `--max-frames N` runs an example for a deterministic number of frames and then
  exits cleanly (0 = interactive), which is what the validation smoke runs use.

## Runtime add/remove/enable

```cpp
pipeline.SetPassEnabled(handle, false);  // skipped from the next frame
pipeline.RequestRebuild();               // after external edits, if needed
```

`RemovePass(handle)` is deferred until the FIF-deep pass is no longer in
flight. Enable/disable and add/remove take effect at the next `ApplyChanges()`.

## Resize

Registered passes can react to extent changes for app-owned resources:

```cpp
void OnRenderResize(std::uint32_t width, std::uint32_t height) override;
```

The engine calls it once per change at a frame boundary (drained by
`ApplyChanges()`), never during execution. Graph-declared relative transients
are resized by the engine automatically.

## Built-in anchors

`BuiltinPass` enumerates the engine passes for ordering: `Expand`,
`OccluderSelect`, `OccluderPrepass`, `HiZGenPre`, `PreCull`, `DepthPrepass`,
`HiZGen`, `Occlusion`, `Collect`, `MainPass`, `ImGui`. Engine descriptor sets
0-4 are reserved; application sets start at `kFirstAppDescriptorSet` (5).
