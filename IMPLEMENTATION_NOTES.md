# Implementation Notes

This document tracks implementation choices for the runtime and render-graph foundation. It is intended to be updated continuously while the architecture is being built.

## Scope of Current Work

- Build a practical v1 render-graph core that compiles graph intent into a deterministic execution plan.
- Keep runtime/platform concerns separated from graph logic.
- Prefer correctness and diagnostics over early optimization.

## Architectural Decisions (Current)

### 1. Render graph starts as a compile-time reasoning core

The first concrete implementation lives in:

- `src/VulkanEngine/RenderGraph/RenderGraph.cppm`
- `src/VulkanEngine/RenderGraph/RenderGraph.cpp`

And follows the codebase module style:

- `RenderGraph.cppm` is the exported interface unit (`VulkanEngine.RenderGraph`)
- `RenderGraph.cpp` is the module implementation unit (`module VulkanEngine.RenderGraph;`)
- tests import the module directly instead of including a header

This provides:

- typed pass/resource handles with generation values
- imported vs transient resource declaration
- pass declaration with queue intent + enable/disable
- read/write resource usage declaration
- optional explicit pass dependencies
- compile step with:
  - dependency edge construction
  - topological sort
  - cycle detection
  - resource lifetime interval extraction
  - diagnostics reporting

### 1.1 Structured diagnostics and state model are now part of the base API

The render-graph API now includes:

- `DiagnosticCode` values for machine-readable compile outcomes
- structured diagnostics in `CompileDiagnostic` (`code + severity + message`)
- explicit `ResourceState` model with:
  - stage intent
  - access intent
  - queue intent
  - optional image layout intent
- resource-level `SetInitialState` / `SetFinalState` hooks

This keeps sync2-oriented planning extensible without API churn.

### 1.2 Runtime-to-graph bridge contract introduced

Added module:

- `src/VulkanEngine/RenderGraph/GraphExecutionContext.cppm`
- `src/VulkanEngine/RenderGraph/GraphExecutionBridge.cppm`

It defines:

- `BorrowedCommandContext`
- `ImportedFrameResources`
- `GraphExecutionContext`

And bridge helper:

- `CreateGraphExecutionContext(RuntimeFrameInfo, ImportedFrameResources, BorrowedCommandContext)`

This establishes a narrow frame contract between runtime and graph layers.

### 1.3 Minimal runtime shell module introduced

Added:

- `src/VulkanEngine/Runtime/RuntimeShell.cppm`
- `src/VulkanEngine/Runtime/RuntimeShell.cpp`

The shell currently provides status plumbing for:

- resize pending
- swapchain out-of-date
- swapchain suboptimal
- minimized window state
- shutdown request

This is intentionally minimal and runtime-policy focused.

### 1.4 SDL3 platform shell bootstrap module introduced

Added:

- `src/VulkanEngine/Platform/SdlPlatformShell.cppm`
- `src/VulkanEngine/Platform/SdlPlatformShell.cpp`

The module is backend-injected and currently targets deterministic lifecycle/event-state handling:

- initialize / shutdown flow
- window-create success/failure propagation
- event polling for quit/resize/minimize/restore
- platform status + state snapshots for runtime orchestration

### 1.5 Vulkan runtime bootstrap skeleton module introduced

Added:

- `src/VulkanEngine/Runtime/VulkanBootstrap.cppm`
- `src/VulkanEngine/Runtime/VulkanBootstrap.cpp`

Current skeleton scope:

- ordered bootstrap stages (instance, device selection, logical device, swapchain)
- typed bootstrap status reporting
- frame-begin status behavior
- swapchain out-of-date notification + recreation hook
- device-lost notification behavior

The backend is injected for deterministic testing while Vulkan object ownership remains a later phase.

### 1.6 Concrete backend wiring added behind the bootstrap interfaces

Added concrete backend modules:

- `src/VulkanEngine/Platform/SdlPlatformBackend.cppm`
- `src/VulkanEngine/Platform/SdlPlatformBackend.cpp`
- `src/VulkanEngine/Runtime/VulkanBootstrapBackend.cppm`
- `src/VulkanEngine/Runtime/VulkanBootstrapBackend.cpp`

Current behavior:

- SDL backend: initializes SDL video, creates a Vulkan-capable resizable window, translates SDL events into `PlatformEvent`.
- Vulkan backend: uses `volk` to initialize Vulkan, create instance, select a physical device, create logical device + graphics queue, and provide a skeleton swapchain-image-count stage.

This keeps the architecture boundaries from prior steps while introducing real backend plumbing.

### 1.7 App bootstrap loop now uses the real backend factories

Updated:

- `src/app/main.cpp`

Current app behavior:

- create concrete SDL backend and initialize platform shell
- create concrete Vulkan backend and initialize bootstrap skeleton
- run runtime/platform polling loop
- handle resize -> swapchain out-of-date -> recreate flow
- propagate frame data through `CreateGraphExecutionContext(...)`
- shutdown runtime/bootstrap/platform cleanly

### 1.8 App demo rendering path added (spinning textured object)

Updated:

- `src/app/main.cpp`

Added behavior:

- routes frame presentation through Vulkan swapchain backend (`RenderFrameSoftware`)
- loads first available model from `models/` in runtime output directory
- loads first available BMP texture from `textures/` in runtime output directory
- CPU-rasterizes textured spinning mesh to RGBA and presents through Vulkan
- uses exception-based failure handling for model/texture/context setup failures

### 1.10 Vulkan backend reliability fixes landed

Recent runtime fixes include:

- corrected backend initialization control-flow bugs in `CreateInstance` and device selection
- swapchain extent handling for platforms returning undefined `currentExtent`
- swapchain image layout transition tracking per image
- safer present synchronization for demo path
- richer bootstrap failure visibility in app log output

### 1.9 Asset copy helper added for app runtime folders

Added:

- `cmake/CopyAppAssets.cmake`

Integrated in:

- `src/app/CMakeLists.txt`

Current behavior mirrors `src/app/models` and `src/app/textures` into the app binary directory after build and reconfigures when source assets change.

### 2. Conservative dependency model

The current model intentionally prefers safety:

- read after write creates ordering edge
- write after read creates ordering edge
- write after write creates ordering edge
- explicit pass dependencies are also respected

This is enough to establish correctness in a single-queue baseline while leaving room for later queue-aware expansion.

### 3. Disabled pass pruning is compile-time

Disabled passes are pruned during compile, not execution. This keeps runtime execution deterministic and lightweight.

### 4. Initial test contract

`tests/render_graph_tests.cpp` covers:

- deterministic ordering through resource dependencies
- cycle detection from explicit dependency loops
- disabled pass pruning

These tests are a minimum quality gate before integrating with runtime command recording.

## Deferred by Design (Still deferred)

- Vulkan command recording integration
- synchronization2 barrier synthesis
- dynamic rendering attachment emission
- multi-queue scheduling
- transient alias allocator backend

These remain planned but deferred until the compile-only graph core is stable.

## Change Log

- Added initial render-graph core API and compiler implementation.
- Converted render-graph API to an exported C++ module interface unit and switched tests to module import usage.
- Added first render-graph unit tests.
- Integrated render-graph source/test files into build system.
- Added structured diagnostic codes and explicit resource-state model types.
- Added `GraphExecutionContext` runtime-bridge contract module.
- Added a minimal `RuntimeShell` module with resize/out-of-date/suboptimal/minimized status plumbing.
- Added unit tests for diagnostics, execution context contract, and runtime-shell status behavior.
- Added `SdlPlatformShell` platform bootstrap module.
- Added `VulkanBootstrap` runtime bootstrap skeleton module.
- Added unit tests for platform shell and Vulkan bootstrap stage/status behavior.
- Added concrete SDL3 and volk-based Vulkan backends with module-first interfaces and factory entry points.
- Added backend factory tests to validate concrete backend creation paths.
- Wired app bootstrap loop to the concrete backend factory modules.
- Added app asset copy CMake helper and app-level textured spinning demo rendering path.
- Validated with:
  - `cmake --build build --config Debug --target example_tests`
  - `ctest --test-dir build -C Debug --output-on-failure`

## Near-Term Next Steps

1. Add richer diagnostic payload fields (pass/resource identifiers) for improved tooling output.
2. Add validation paths for unsupported usage combinations in compile diagnostics.
3. Move demo rendering from OpenGL path to Vulkan swapchain path while preserving current module boundaries.
4. Add explicit runtime error logging/diagnostics payload surfaces for backend-stage failures and asset-loading failures.









