# Descriptor rewiring contract (locked in Phase 5)

This contract exists so transient reallocation (Phase 8) does not require a
descriptor-system redesign. Treat it as locked unless profiling forces a change.

## Decision

When a transient resource is reallocated (heap growth, alias-plan rebuild, or a
pass reconfiguration), the engine **rewrites the affected descriptor writes**
rather than relying on descriptor indexing / bindless indirection.

## Requirements

- **One binding-owner per pass.** A pass declares its descriptors through
  `PassSetupContext::DeclareBindings` (`VulkanEngine::Render::DescriptorDecl`).
  The engine composes the layout; the pass never creates `VkDescriptorSetLayout`
  or writes descriptors by hand.
- **Per-FIF descriptor sets.** App-pass sets are allocated once per declared set
  per frames-in-flight slot, so a rewrite only ever targets the slot being
  recorded and never a set an in-flight frame still reads. The transient rewrite
  path therefore does not require update-after-bind pools (the bindless arrays
  keep their own update-after-bind pool where required). If a set ever must be
  rewritten while in flight, allocate it from an update-after-bind pool with
  matching layout/pool flags (VUID-VkDescriptorSetAllocateInfo-descriptorPool-00308).
- **Partial binding.** Descriptor arrays that grow over time (per-material
  blocks) use `VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT` so unwritten entries
  are legal.
- **Rewire point.** A frame's descriptor sets are rewritten from the resources
  resolved that frame while its command buffers are recorded, after the alias
  plan and transient heap have been reconciled. A rewrite must never target a
  set that an in-flight frame may still read: per-FIF ownership guarantees that
  for the transient path, otherwise use update-after-bind or defer until the
  previous owner is GPU-complete (`IsFrameComplete`).
- **Bounds.** Rewriting must keep every written range inside the declared
  `count`; the engine validates this at declaration time via
  `PipelineLayoutComposer` (reserved sets, duplicates, set gaps, push ranges).

## Why rewrite per-pass descriptors instead of routing transients through bindless

Feature availability is **not** a constraint: the engine already requires and
enables the full Vulkan 1.2 descriptor-indexing set (`descriptorIndexing`,
`runtimeDescriptorArray`, `descriptorBindingPartiallyBound`,
`descriptorBindingSampledImageUpdateAfterBind`,
`descriptorBindingStorageBufferUpdateAfterBind`, the non-uniform-indexing
variants, and `variableDescriptorCount`), and `BindlessManager` already uses a
runtime descriptor array with an update-after-bind pool. Descriptor indexing
therefore stays the right tool for the texture/material bindless arrays.

The rewrite path is chosen for transient resources for behavioral reasons:

- **A rewrite is required anyway.** Transients are aliased: the same memory is
  viewed as different images over time, and the alias plan / heap can change on
  reconfiguration. A stable bindless index would still have its descriptor
  rewritten whenever the binding changes, so descriptor indexing does not
  remove the update.
- **Bounded ownership.** Bindless slots are a globally capped resource
  (`maxDescriptorSet*SampledImages`, etc.) shared with texture/material
  binding. Per-frame transients consuming variable slots from that pool can
  exhaust it; per-pass sets are bounded and owned by the pass.
- **Declaration-time validation.** Per-pass sets let `PipelineLayoutComposer`
  validate reserved sets, duplicates, gaps, and push-constant ranges when the
  pass is declared.

A transient descriptor array (bindless slot with a per-frame base index) remains
a possible future optimization; it is not required by feature availability and
would not remove the need for per-frame descriptor writes.

## Offset alignment

Buffer bindings must honor `minStorageBufferOffsetAlignment` and
`minUniformBufferOffsetAlignment`; the binding helpers
(`MakeStorageBufferBinding` / `MakeUniformBufferBinding`) enforce this and are
unit-tested. Sampled image bindings require a resolved view.
