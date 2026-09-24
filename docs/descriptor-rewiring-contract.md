# Descriptor rewiring: decision and rationale

## Decision

When a transient resource is reallocated (heap growth, alias-plan rebuild, or a
pass reconfiguration), the engine **rewrites the affected descriptor writes**. It
does not route transients through bindless indirection.

## Requirements

- **One binding-owner per pass.** A pass declares its descriptors; the engine
  composes the layout. The pass never creates a descriptor-set layout or writes
  descriptors by hand.
- **Per-frames-in-flight descriptor sets.** App-pass sets are allocated once per
  declared set per frames-in-flight slot, so a rewrite only ever targets the slot
  being recorded and never a set an in-flight frame still reads. If a set must
  ever be rewritten while in flight, allocate it from an update-after-bind pool
  with matching layout and pool flags
  (VUID-VkDescriptorSetAllocateInfo-descriptorPool-00308).
- **Partial binding.** Descriptor arrays that grow over time (for example
  per-material blocks) use `VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT` so
  unwritten entries are legal.
- **Rewire point.** A frame's descriptor sets are rewritten from the resources
  resolved that frame while its command buffers are recorded, after the alias plan
  and transient heap have been reconciled. A rewrite must never target a set that
  an in-flight frame may still read: per-frames-in-flight ownership guarantees
  that for the transient path, otherwise use update-after-bind or defer until the
  previous owner is GPU-complete.
- **Bounds.** A rewrite keeps every written range inside the declared count. The
  engine validates this at declaration time (reserved sets, duplicates, set gaps,
  push-constant ranges).

## Why rewrite rather than route transients through bindless

Descriptor indexing is available and is the right tool for the texture and
material bindless arrays. For transient resources, rewrites win for behavioral
reasons:

- **A rewrite is required anyway.** Transients are aliased: the same memory is
  viewed as different images over time, and the alias plan or heap can change on
  reconfiguration. A stable bindless index still needs its descriptor rewritten
  whenever the binding changes, so descriptor indexing does not remove the update.
- **Bounded ownership.** Bindless slots are a globally capped pool shared with
  texture and material binding. Per-frame transients consuming variable slots from
  that pool can exhaust it; per-pass sets are bounded and owned by the pass.
- **Declaration-time validation.** Per-pass sets let the layout composer validate
  reserved sets, duplicates, gaps, and push-constant ranges when the pass is
  declared.

A transient descriptor array (a bindless slot with a per-frame base index) remains
a possible future optimization. It is not required by feature availability and it
would not remove the need for per-frame descriptor writes.

## Offset alignment

Buffer bindings must honor `minStorageBufferOffsetAlignment` and
`minUniformBufferOffsetAlignment`. Sampled image bindings require a resolved view.
