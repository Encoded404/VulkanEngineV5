# Base Implementation Progress

This document tracks milestone-level progress for the base runtime + render-graph implementation.

## Milestone Checklist

- [x] Render-graph module interface and compile core
- [x] Deterministic ordering, cycle detection, and resource lifetime extraction
- [x] Unit tests for core compile behavior
- [x] Structured diagnostic codes in compile output
- [x] Explicit resource state model types in public graph API
- [x] Runtime-to-graph execution context contract module
- [x] Minimal runtime shell with resize/suboptimal/out-of-date status plumbing
- [x] Unit tests for diagnostics, execution context, and runtime shell
- [x] Thin integration adapter: `RuntimeFrameInfo -> GraphExecutionContext`
- [x] SDL3 platform shell bootstrap module
- [x] Vulkan runtime bootstrap module skeleton
- [x] Concrete SDL3 backend wiring behind `SdlPlatformShell` interface
- [x] Concrete Vulkan backend wiring behind `VulkanBootstrap` interface
- [x] App bootstrap loop wired to concrete backend factories
- [x] Asset-copy CMake helper for `src/app/models` and `src/app/textures`
- [x] Runnable spinning textured object demo loop in app
- [x] Demo frame presentation routed through Vulkan backend swapchain path

## Notes

- Implementation style remains module-first (`.cppm` + implementation units).
- `.idea/` is intentionally ignored as user-local IDE state.
- Current runtime shell is policy/state plumbing only; no Vulkan object ownership yet.






