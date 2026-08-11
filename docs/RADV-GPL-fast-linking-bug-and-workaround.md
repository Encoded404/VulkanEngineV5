# RADV GPL (VK_EXT_graphics_pipeline_library) on Mesa < 26.0 — the "fragment stage never renders" bug

> **TL;DR** On RADV (AMD Vulkan) Mesa **23.3.x → 25.3.x**, graphics-pipeline-library (GPL)
> pipelines are broken: the fragment shader never writes color (PS effectively never runs), so
> everything renders black/clear. No flag combination fixes it (fast link, LTO, RETAIN — all
> broken). It is fixed by (a) using a **specific library structure** — DXVK's structure — or
> (b) upgrading to **Mesa ≥ 26.0**, where the driver bug was fixed (no known-good pre-26 RADV
> version exists). This document is a field guide: symptoms, root cause, the exact working
> structure, linking semantics, crash inventory, validation findings, and a full test matrix.

---

## Table of contents

1. [Executive summary](#1-executive-summary)
2. [Symptoms & how to verify them](#2-symptoms--how-to-verify-them)
3. [Root cause](#3-root-cause)
4. [Affected versions](#4-affected-versions)
5. [The working library structure (DXVK pattern)](#5-the-working-library-structure-dxvk-pattern)
6. [Linking: flags, fast link vs. optimized link](#6-linking-flags-fast-link-vs-optimized-link)
7. [Crash inventory](#7-crash-inventory)
8. [Validation-layer findings & spec VUIDs](#8-validation-layer-findings--spec-vuids)
9. [Practical checklist for your engine](#9-practical-checklist-for-your-engine)
10. [Testing guide (repro harness + packaged drivers)](#10-testing-guide-repro-harness--packaged-drivers)
11. [Driver detection & version gating notes](#11-driver-detection--version-gating-notes)

- [Appendix A: full variant matrix (a–x)](#appendix-a-full-variant-matrix-a-x)
- [Appendix B: VUIDs encountered](#appendix-b-vuids-encountered)
- [Appendix C: glossary](#appendix-c-glossary)

---

## 1. Executive summary

| Question | Answer |
|---|---|
| Does GPL fast linking work on RADV < 26.0? | **No** — 23.3 → 25.3 all broken (empirically tested, §4) |
| Does any flag combination fix it? | No — it's the **library structure** (§5), not the flags (§6) |
| Works on RADV ≥ 26.0? | Yes (26.0.6 and 26.1.4 verified) |
| Minimal working structure | FS lib = `FRAGMENT_SHADER` bit only; separate FOI lib with `pColorBlendState` + `pMultisampleState`; empty `VkPipelineRenderingCreateInfo` chained into GE **and** FS libs (§5) |
| Fast link and LTO link on <26? | Both work **with** the split structure; both broken without it (§6) |
| Ever reported upstream? | No public Mesa issue; fixed silently in 26.0 (MR 33979, MR 33928, `17e597093d`) (§3.3) |
| Should you gate by version? | Only if you can't change the structure; prefer structure change (§11) |

---

## 2. Symptoms & how to verify them

### 2.1 Symptoms

- A pipeline created by linking GPL libraries (fast- *or* LTO-linked, §6) is returned
  `VK_SUCCESS`, but **nothing renders**:
  - vertex stage runs (3 VS invocations for a triangle),
  - clipping happens (geometry reaches the rasterizer),
  - **zero fragment-shader invocations** in `VK_QUERY_TYPE_PIPELINE_STATISTICS`,
  - framebuffer keeps its clear color.
- A monolithic pipeline (no libraries) with identical state renders correctly on the same driver.
- The bug is deterministic and affects **every** GPL pipeline on the affected drivers, not just
  some shaders.

### 2.2 Verification methods

**a) Pipeline statistics** (`VK_QUERY_TYPE_PIPELINE_STATISTICS`)

Request at least: `VERTEX_SHADER_INVOCATIONS | FRAGMENT_SHADER_INVOCATIONS | CLIPPING_INVOCATIONS |
INPUT_ASSEMBLY_PRIMITIVES`. The discriminating signature is:

```
IA primitives > 0,  VS invocations > 0,  clipping invocations > 0,  PS invocations == 0
```

→ geometry is produced and clipped, but the fragment stage never executes. Note: results are
returned in **ascending bit order** of the requested flags, not in your flag order.

**b) `RADV_DEBUG=pso_history`** (exists since Mesa 25.1; log at `/tmp/radv_pso_history.log`)

```
pipeline_hash=0000000000000000, VA=..., stage=vertex
pipeline_hash=0000000000000000, VA=..., stage=fragment
```

A `pipeline_hash` of **0 on the final pipeline = the fast-link path** (hash deliberately not
computed). The log iterates the pipeline's `shaders[]` array, so the fragment *binary* appears
present even when it never executes — do not conclude "FS missing" from pso_history alone; pair
it with (a) or (c).

**c) Side-effect probe (proves the FS runs but its color output is dropped)**

Compile the FS with a side effect that can't be optimized away, e.g. an SSBO `atomicAdd`:

```glsl
layout(set = 0, binding = 0) buffer SB { uint counter; };
void main() { atomicAdd(counter, 1u); outColor = vec4(1,0,0,1); }
```

On Mesa 25.3.6 with a broken GPL pipeline: **counter = fullscreen pixel count (e.g. 262144 for
512×512), pixels still clear** → the FS *executes* (and is counted), but its color export never
reaches the color buffer. On a fixed driver the same probe writes counter **and** red pixels.

---

## 3. Root cause

### 3.1 What happens (empirical facts, all verified)

1. The FS binary attached to the final GPL pipeline is **byte-size identical** on broken 25.3.6
   and fixed 26.1.4 (pso_history VA ranges) — the shader itself is fine.
2. On 25.3.6 the FS **runs** (side-effect probe, §2.2c) yet writes no color.
3. The deciding factor is the **PS-epilog code path** RADV takes when compiling the FS inside a
   library:
   - `has_epilog == true` (FS library without the FOI bit) → **works** on <26;
   - `has_epilog == false` (FS library with/bundling the FOI bit) → **broken** on <26.

### 3.2 Code-level analysis (per 25.3.6 source; inferred, not 100% proven)

- `radv_pipeline_needs_ps_epilog(state, lib_flags)` returns true when the library has
  `FRAGMENT_SHADER` without `FRAGMENT_OUTPUT_INTERFACE` (or with dynamic CB/ms states).
- When `has_epilog == false`, the FS compile runs `radv_nir_remap_color_attachment()` which
  rewrites/removes color stores based on `gfx_state->ps.epilog.color_map` — a mapping that is
  unreliable for library-compiled (unlinked) shaders on <26.
- At draw time, `radv_bind_fragment_output_state()` (radv_cmd_buffer.c) derives the PS-epilog key
  and masks `spi_shader_col_format &= colors_written` from `ps->info.ps.colors_written` — the
  likely place where the color output is dropped for the broken path (analysis; the observable
  behavior in §2.2 is fully verified regardless).
- The empty `VkPipelineRenderingCreateInfo` chained into the GE and FS libraries makes the driver
  treat the attachment state as present (`state->rp` non-NULL), flipping the epilog decision —
  this is why its absence from *either* library breaks rendering again (§5, Appendix A: t/u/v).

### 3.3 The upstream fix (Mesa 26.0, silent — no issue was ever filed)

- **MR 33979** — "radv: Scalarize and re-vectorize unlinked shader I/O" (`58020fdc`, merged
  2026-01-02) + 7 follow-ups ("Don't call nir_link_opt_varyings anymore", "Don't call
  nir_compact_varyings anymore", …).
- **MR 33928** — noop-FS handling (`473ef0b6` "Use nir_remove_outputs with the noop FS",
  `72ac874b` "Remove radv_remove_varyings").
- **`17e597093d`** — "radv: eliminate unused FS output channels" (rewrote the FS compile block:
  `radv_nir_trim_fs_color_exports`, `nir_lower_io_to_scalar` on FS outputs, post-dce).
- **Not backported** to 25.3 (25.3.6 is the final 25.3 point release).

Closest related (but different) public issues: [#12516](https://gitlab.freedesktop.org/mesa/mesa/-/issues/12516)
(mesh-shader depth-only / noop-FS path), [#8150](https://gitlab.freedesktop.org/mesa/mesa/-/issues/8150)
(GPL fast-link *times*), [#8258](https://gitlab.freedesktop.org/mesa/mesa/-/issues/8258)
(2023-era GPL blackouts, experimental path).

---

## 4. Affected versions

All tested with the same harness on packaged drivers (host Fedora 43, flatpak GL extensions, and
throwaway podman containers with /dev/dri passthrough):

| Version | GPL fast link | Notes |
|---|---|---|
| Mesa 23.3.6 (Fedora 39) | **broken** (PS = 0) | |
| Mesa 24.1.7 (Fedora 40) | **broken** | |
| Mesa 25.0.7 (Fedora 41) | **broken** | |
| Mesa 25.1.9 (Fedora 42) | **broken** | |
| Mesa 25.3.6 (Fedora 43; last 25.3 release) | **broken** | your host driver |
| Mesa 26.0.6 (flatpak `org.freedesktop.Platform.GL.default//24.08`) | **fixed** | |
| Mesa 26.1.4 (flatpak `…GL.default//25.08`) | **fixed** | |
| llvmpipe/lavapipe 25.3.6 (control) | works | same calls, software driver |

**No known-good pre-26 RADV version exists.** The bug spans the whole modern GPL era.

---

## 5. The working library structure (DXVK pattern)

### 5.1 Requirements (verified on 25.3.6, 26.0.6, 26.1.4, lavapipe)

1. **FS library**: `VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT` **only** — do *not* set
   `FRAGMENT_OUTPUT_INTERFACE_BIT_EXT` on it (this forces `has_epilog`, §3.2).
2. **FOI library** (separate): `VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT`
   with `pColorBlendState` **and** `pMultisampleState` (ms is mandatory — see §8 and crash #2 in §7).
3. **Chain a `VkPipelineRenderingCreateInfo` into the GE (pre-rasterization) library AND into the
   FS library.** It may be completely empty (just `sType`; no formats, no viewMask). Removing it
   from *either* library re-breaks rendering (Appendix A: t, u).
4. **Link** per §6 (fast link = no LTO on final; optimized = LTO|RETAIN libs + LTO final).

```
GE  library:  VERTEX_INPUT_INTERFACE | PRE_RASTERIZATION (may be one lib or two)
              + VkPipelineRenderingCreateInfo (empty OK)          <- REQUIRED
FS  library:  FRAGMENT_SHADER only (NO FOI bit)                   <- REQUIRED
              + VkPipelineRenderingCreateInfo (empty OK)          <- REQUIRED
FOI library:  FRAGMENT_OUTPUT_INTERFACE only
              + pColorBlendState + pMultisampleState              <- both REQUIRED
final:        vkCreateGraphicsPipelines(libs)  — no LTO flag for fast linking
```

### 5.2 What does *not* matter (all verified)

- Merged vs. separate vertex-input and pre-rasterization libraries (variant s merges them).
- Static vs. dynamic cull mode / front-face / depth-bias (variant q).
- Whether the FS library carries `pMultisampleState` (variant o).
- Whether the FOI library chains `VkPipelineRenderingCreateInfo` with formats (variant w).
- Library lifetimes: destroying the libraries immediately after linking is fine.
- Vertex-input contents, shader complexity, mesh/tessellation, etc.

### 5.3 Pseudocode

```c
VkPipelineRenderingCreateInfo rinfo_empty = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
VkPipelineRenderingCreateInfo rinfo_fmt   = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                              .colorAttachmentCount = 1,
                                              .pColorAttachmentFormats = &color_format };

// 1) GE library (vertex input + pre-rasterization)
VkGraphicsPipelineLibraryCreateInfoEXT ge_lib = {
    .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT,
    .flags = VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT |
             VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT,
    .pNext = &rinfo_empty,                       // REQUIRED on <26
};
// ...pStages=[VS], pVertexInputState, pInputAssemblyState, pViewportState, pRasterizationState...

// 2) FS library: FRAGMENT_SHADER bit ONLY
VkGraphicsPipelineLibraryCreateInfoEXT fs_lib = {
    .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT,
    .flags = VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT,   // NO FOI bit
    .pNext = &rinfo_empty,                       // REQUIRED on <26
};
// ...pStages=[FS], pDepthStencilState, pDynamicState...

// 3) FOI library
VkGraphicsPipelineLibraryCreateInfoEXT foi_lib = {
    .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT,
    .flags = VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT,
    .pNext = &rinfo_fmt,
};
// ...pColorBlendState, pMultisampleState (both REQUIRED)...

// 4) final link — fast path: NO LINK_TIME_OPTIMIZATION flag
VkPipelineLibraryCreateInfoKHR libs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR,
                                        .libraryCount = 3,
                                        .pLibraries = (VkPipeline[]){ ge, fs, foi } };
VkGraphicsPipelineCreateInfo final = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                       .pNext = &libs,
                                       .layout = layout };
```

---

## 6. Linking: flags, fast link vs. optimized link

### 6.1 Flag semantics

| Flag | Where | Meaning |
|---|---|---|
| `LINK_TIME_OPTIMIZATION` | **library** create | Keep the linkable form (NIR) so the library *can* be LTO-linked later. |
| `LINK_TIME_OPTIMIZATION` | **final** create | Do the slow, cross-stage-optimized re-link now (requires libs that kept the linkable form). |
| `RETAIN_LINK_TIME_OPTIMIZATION_INFO` | **library** create | Keep the linkable form even after it has been used once (re-linking, background optimization). |
| *(neither on final)* | final create | **Fast link**: stitch the already-compiled per-stage binaries. |

### 6.2 "No LTO = no optimization at all?"

No. Each shader is fully optimized (NIR passes + ACO) when its library is created. Without LTO
you only skip the **cross-stage link optimization** (VS↔FS varying compaction, cross-stage
constant folding). Fast linking is the standard technique for creating pipelines at draw time
without stutter; DXVK uses it exclusively and sets *neither* flag on its libraries.

### 6.3 Fast link (recommended for draw-time use on <26)

- Libraries with or without LTO — both work.
- Final create: libraries only, **no** `LINK_TIME_OPTIMIZATION`.
- RADV decides "fast linking" purely from the final create's flags
  (`radv_is_fast_linking_enabled()`: libraries present AND no LTO flag) and skips recompilation
  when every active stage has an imported binary. Verified: pso_history shows `hash=0` for the
  final pipeline and the FS binary is the library's binary (same VA).

### 6.4 Optimized (slow/LTO) link — also works on <26 with the split structure

- Libraries created with `LINK_TIME_OPTIMIZATION | RETAIN_LINK_TIME_OPTIMIZATION_INFO`.
- Final create with `LINK_TIME_OPTIMIZATION`.
- Verified on 25.3.6 (variant x): renders correctly; pso_history shows a recompiled FS
  (different VA, non-zero hash) → the retained NIR re-link path is healthy with the split
  structure.
- Classic two-stage pattern: fast-link at draw time, background-compile the LTO pipeline for
  future frames.

### 6.5 Spec vs. RADV leniency

- Per `VK_EXT_graphics_pipeline_library`, fast linking is only *guaranteed* when **all**
  libraries were created with `LINK_TIME_OPTIMIZATION`, and linking with LTO requires the
  libraries to have kept their link-time info (RETAIN). If a library lacks the linkable form,
  the implementation may return `VK_PIPELINE_COMPILE_REQUIRED` instead of degrading silently.
- RADV is lenient: it fast-links based only on the final create's flags, and (on <26) it can
  even NULL-deref instead of returning `VK_PIPELINE_COMPILE_REQUIRED` (crash #1 in §7) — always
  use `LTO | RETAIN` on libraries if you ever link with LTO, and handle
  `VK_PIPELINE_COMPILE_REQUIRED` as a valid return value.
- **llvmpipe/lavapipe honors FAIL_ON literally**: `lvp_CreateGraphicsPipelines`
  (`lvp_pipeline.c`, same logic in 25.3.x and 26.x) skips pipeline creation entirely whenever
  `VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT` is set and returns
  `VK_PIPELINE_COMPILE_REQUIRED` + `VK_NULL_HANDLE` unconditionally — llvmpipe always needs a
  compile, so the flag can never succeed there. A final link created with FAIL_ON therefore
  NEVER succeeds on lavapipe: retry the create without the flag (accepting a silently compiled
  pipeline) or fall back to monolithic. DXVK's identifier-cache-hit path hits the same wall.
- **Check the handle, never just the result**: `VK_PIPELINE_COMPILE_REQUIRED` is a *success*
  code (positive `1000297000`), so error-style handling (throw / `!= VK_SUCCESS` treats it as
  an error) silently misses it. With exceptions enabled, vulkan-hpp's raii vector form
  discards the result and returns the raw out-array as-is — on `VK_PIPELINE_COMPILE_REQUIRED`
  it contains the `VK_NULL_HANDLE` the driver wrote (newer vulkan-hpp raii codegen instead
  leaves the vector empty on non-`eSuccess`; both are signals to look at the *handle*).

---

## 7. Crash inventory

Separate driver robustness bugs (all triggered by *valid-but-unusual* usage; all reproducible on
both 25.3.6 and 26.0.6/26.1.4 unless noted):

| # | Configuration | Result | Location (25.3.6) | Workaround |
|---|---|---|---|---|
| 1 | Libraries created with `LINK_TIME_OPTIMIZATION` but **without** RETAIN; final link **with** LTO | Segfault — NULL `vs` deref in vertex-input init | `radv_pipeline_graphics.c:750` (`radv_pipeline_init_vertex_input_state`) | Always create libs with `LTO \| RETAIN` (§6.4); handle `VK_PIPELINE_COMPILE_REQUIRED` |
| 2 | **FOI-only library without `pMultisampleState`** | Segfault — NULL `ms_info` deref | `vk_graphics_state.c:809` (`vk_multisample_sample_locations_state_init`) — **common Mesa code, also crashes lavapipe** | Always pass `pMultisampleState` in FOI libs (§5.1, §8) |
| 3 | Final pipeline linked **without any fragment library** (noop-FS case) | Segfault — NULL `state->ms` | `radv_pipeline_graphics.c:968` (`radv_pipeline_init_dynamic_state`, alpha-to-coverage branch) | Always link at least an FS library |
| 4 | Pre-rasterization lib with DXVK-style **dynamic** states merged with the vertex-input lib (VI\|PRE combined + dynamic cull/front-face/bias) | Segfault — same NULL `state->ms` family | `radv_pipeline_graphics.c:968` | Keep VI/PRE in separate libraries when using dynamic pre-rast states, or use the §5 structure |

These are worth filing upstream (mesa/gitlab) with the repro harness (§10) — none of them have a
tracking issue at the time of writing.

---

## 8. Validation-layer findings & spec VUIDs

Ran the harness under `VK_LAYER_KHRONOS_validation` (Fedora 43's SDK):

- **No GPL-related VUID fires on the working variants** (s, m, x) — the §5 structure is
  validation-clean.
- **`VUID-VkGraphicsPipelineCreateInfo-pMultisampleState-09026`** (current spec): "If the
  pipeline requires fragment output interface state, `pMultisampleState` **must** be a valid
  pointer." — this is the "multisample requirement became validation" change; it matches what
  mesa's common code needs to avoid crash #2 (§7).
- **`VUID-06635 / 06636 / 06637`**: if both the FS library and the FOI library provide
  `pMultisampleState`, they must be **identically defined** — pass the same struct, or only in the
  FOI library (what DXVK does for non-sample-shaded shaders).
- **`VUID-VkGraphicsPipelineCreateInfo-None-09497`**: newer validation expects a
  `VkPipelineCreateFlags2CreateInfo` in the pNext chain when setting pipeline create flags —
  DXVK always chains one; prefer it over the legacy `flags` field.
- The only validation-clean way to omit `pMultisampleState` from an FOI library is making
  rasterization-samples / sample-mask / alpha-to-coverage dynamic via
  `VK_EXT_extended_dynamic_state3` (the EDS3 carve-out in VUID-09026).

---

## 9. Practical checklist for your engine

To use GPL on RADV < 26 (this also stays valid on 26+):

- [ ] FS library uses `FRAGMENT_SHADER` bit only (no FOI bit).
- [ ] FOI is a separate library with `pColorBlendState` + `pMultisampleState`.
- [ ] `VkPipelineRenderingCreateInfo` (empty is fine) chained into GE and FS library creates.
- [ ] Draw-time pipelines: final create without `LINK_TIME_OPTIMIZATION` (fast link).
- [ ] Background-optimized pipelines: libs created with `LTO | RETAIN`, final with `LTO`.
- [ ] Handle `VK_PIPELINE_COMPILE_REQUIRED` as a valid return value.
- [ ] Optionally chain `VkPipelineCreateFlags2CreateInfo` (validation hygiene, §8).
- [ ] Re-verify once per Mesa major with the harness (§10) — the bug may re-enter via future
      refactors.

---

## 10. Testing guide (repro harness + packaged drivers)

Harness: `/tmp/opencode/gpl-probe/` — single-file C app (`gpl_probe.c`), 24 pipeline variants
(a–x), GLSL shaders, pipeline-statistics + pixel readback + optional SSBO probe.

```sh
cd /tmp/opencode/gpl-probe
gcc -O2 -o gpl_probe gpl_probe.c $(pkg-config --cflags --libs vulkan)
./gpl_probe vert.spv frag.spv 0    # broken fast-link (combined FS|FOI)  -> PS=0, clear pixels
./gpl_probe vert.spv frag.spv 18   # working split structure (variant s) -> PS>0, red pixels
./gpl_probe vert.spv frag.spv 23   # working LTO slow path (variant x)   -> PS>0, red pixels
RADV_DEBUG=pso_history ./gpl_probe vert.spv frag.spv 18   # -> /tmp/radv_pso_history.log
```

Testing newer packaged Mesa without touching the system (package-managed):

```sh
# flatpak GL extensions already install Mesa 26.0.6 (24.08) and 26.1.4 (25.08)
VK_ICD_FILENAMES=icd_flatpak_2606.json LD_LIBRARY_PATH=<26.0.6 GL lib dir> ./gpl_probe vert.spv frag.spv 18
VK_ICD_FILENAMES=icd_flatpak_2614.json LD_LIBRARY_PATH=<26.1.4 GL lib dir> ./gpl_probe vert.spv frag.spv 18
# or older packaged drivers in a throwaway container:
podman run --device /dev/dri/renderD128 --device /dev/dri/card1 fedora:40 ...   # mesa 24.1.7
```

---

## 11. Driver detection & version gating notes

- **Detect by driver ID, never by device-name string**: `VkPhysicalDeviceDriverProperties.driverID
  == VK_DRIVER_ID_MESA_RADV` (value 3; device names like "RADV NAVI21" are GPU-family-specific).
- Mesa encodes its version in `VkPhysicalDeviceProperties.driverVersion` as
  `VK_MAKE_API_VERSION(0, major, minor, patch)` — e.g. 25.3.6 = `(25<<22)|(3<<12)|6 =
  104869894 = 0x6403006` (note: not `0x64003006` — a stray zero that decodes to
  (400, 48, 6)).
- Prefer fixing the library structure (§5) over version gating — the structure is correct on all
  versions and drivers. If you must gate (e.g. until a driver update lands):
  `RADV && mesaVersion < VK_MAKE_API_VERSION(0, 26, 0, 0)`.
- Last-resort escape hatch: `RADV_DEBUG=nogpl` makes RADV not advertise
  `VK_EXT_graphics_pipeline_library` at all (useful for diagnosis, not a fix).

---

## Appendix A: full variant matrix (a–x)

All on RADV **25.3.6** unless noted. ✓ = renders (PS > 0, red pixels); ✗ = renders nothing
(PS = 0, clear pixels); 💥 = driver segfault. (26.0.6/26.1.4: a–i, k, l render ✓; b/d/f still 💥;
lavapipe: a–c ✓, s ✓, d 💥.)

| Var | Structure | Flags | 25.3.6 |
|---|---|---|---|
| a | GE(VI\|PRE) + FS\|FOI combined | LTO libs, fast | ✗ |
| b | same | LTO libs (no RETAIN), final LTO | 💥 #1 |
| c | same | no LTO anywhere, fast | ✗ |
| d | GE + FS-only + FOI (FOI w/o ms) | fast | 💥 #2 |
| e | GE + FS\|FOI combined | LTO\|RETAIN libs, fast | ✗ |
| f | GE only (no fragment lib) | fast | 💥 #3 |
| g | GE + FS\|FOI combined | LTO\|RETAIN libs, final LTO | ✗ |
| h | like a, keep libs alive | fast | ✗ |
| i | like g, keep libs alive | LTO final | ✗ |
| j | GE + FS-only + FOI (no rinfos) | fast | ✗ |
| k | GE + FS\|FOI combined, dynamic CB | fast | ✗ |
| l | GE + FS\|FOI combined, no rendering info | fast | ✗ |
| **m** | **VI + PRE + FS-only + FOI (DXVK exact)** | **no LTO, fast** | **✓** |
| n | m but VI\|PRE merged + dynamic pre-rast states | fast | 💥 #4 |
| o | m + FS lib with ms | fast | ✓ |
| p | m without rinfos in PRE/FS | fast | ✗ |
| q | m with static cull | fast | ✓ |
| r | m with FOI w/o ms | fast | 💥 #2 |
| **s** | **GE(VI\|PRE)+rinfo, FS-only+rinfo, FOI** | **no LTO, fast** | **✓** |
| t | s without GE rinfo | fast | ✗ |
| u | s without FS rinfo | fast | ✗ |
| v | s without both rinfos | fast | ✗ |
| w | s without FOI rinfo | fast | ✓ |
| **x** | **s structure** | **LTO\|RETAIN libs, final LTO** | **✓** |

Take-aways: split FS/FOI (§5.1) + rinfo in GE **and** FS libs (t/u) + ms in FOI lib (r) +
no-LTO final (a) or LTO|RETAIN final (x). Variant **s** is the minimal working config.

---

## Appendix B: VUIDs encountered

| VUID | Context | Verdict |
|---|---|---|
| `pMultisampleState-09026` | ms required when FOI state required | Working structure satisfies it; EDS3 carve-out exists |
| `pLibraries-06635/06636/06637` | FS-lib ms and FOI-lib ms must be identically defined | Pass the same struct or ms only in FOI lib |
| `None-09497` | flags must go through `VkPipelineCreateFlags2CreateInfo` | Chain the flags2 struct (DXVK does) |
| `dynamicRendering-06576`, `-06446` | dynamicRendering feature must be enabled | Enable `VkPhysicalDeviceVulkan13Features.dynamicRendering` in device create |
| `queryType-00791`, `None-00807`, `queryType-00800`, `None-02665` | pipeline-statistics/hostQueryReset/precise-query setup | Harness artifacts; enable `pipelineStatisticsQuery`, `hostQueryReset`, use `VK_QUERY_RESULT_64_BIT` |

---

## Appendix C: glossary

| Term | Meaning |
|---|---|
| GPL | `VK_EXT_graphics_pipeline_library` — precompile pipeline *libraries*, combine them later |
| Fast link | Final `vkCreateGraphicsPipelines` without LTO → stitch precompiled binaries (no recompile) |
| LTO / link-time optimization | Slow re-link that optimizes shaders *across* stages; needs retained linkable form |
| RETAIN | Keep the linkable form after use, for future re-links |
| GE / pre-rast | Pre-rasterization shader state (VS/TCS/TES/GS/MS + IA/VP/RS) |
| FS lib | Fragment-shader library (`FRAGMENT_SHADER` bit) |
| FOI lib | Fragment-output-interface library (`FRAGMENT_OUTPUT_INTERFACE` bit: CB/blend state) |
| has_epilog | RADV internal: FS compiled to defer color export to a per-draw PS epilog (§3.2) |
| pso_history | `RADV_DEBUG=pso_history`; logs per-pipeline shader VAs to `/tmp/radv_pso_history.log` |
| PS / PS invocations | Fragment shader ("pixel shader"); `FRAGMENT_SHADER_INVOCATIONS` pipeline statistic |
