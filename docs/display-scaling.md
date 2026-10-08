# Display scaling: logical points vs physical pixels

The engine has two coordinate spaces and they are not interchangeable. This
document names them, states the one rule that crosses between them, and records
what happens when the crossing is skipped.

## 1. The two spaces

| | **logical points** | **physical pixels** |
|---|---|---|
| produced by | `SDL_GetWindowSize`, every SDL input event | `SDL_GetWindowSizeInPixels`, `VkSurfaceCapabilitiesKHR` |
| configured by | `PlatformConfig::window_width/height` | derived — never a caller's input |
| consumed by | UI sizes a caller writes, ImGui layout, mouse positions | the swapchain extent, `frame.render_extent`, viewport/scissor, the glyph rasterizer, `TextPass` |

The ratio between them is `PlatformState::content_scale`: physical pixels per
logical point. It is `1.0` wherever the platform reports no scale.

## 2. Why the window flag is not optional

The scale is not something the engine can ask for and then act on. On Wayland,
SDL's `GetWindowScale()` returns **1.0** unless `SDL_WINDOW_HIGH_PIXEL_DENSITY`
was passed to `SDL_CreateWindow` — the flag gates the entire scale path,
including `wl_surface_set_buffer_scale` and the viewporter. `SDL_GetWindowDisplayScale()`
returns `window->display_scale`, which is computed from that same gated value, so
it answers `1.0` for the very window whose scale you are asking about.

The order is therefore fixed: pass the flag, *then* read the scale. A design that
queries the scale to decide whether the flag is needed cannot work.

When the flag is absent on a scaled output, the backbuffer SDL hands the engine is
the window's size in points. The engine builds a swapchain at that size and the
compositor stretches every pixel to the output. This is most visible on text:
`docs/text-rendering.md` §1.1 explains that the screen path is hinted and drawn
1:1 by construction, and a compositor upscale resamples exactly the coverage
hinting produced. The blur is the symptom; the flag is the cause.

## 3. The one crossing

`ToPhysicalPixels()` (`VulkanEngine.Text.Layout`) is the only place points become
pixels for screen text. `TextSystem::SubmitScreenText` takes `point_size` and a
point-space origin, converts once at its boundary, and everything below it —
layout, rasterization, the queued instances — is in pixels.

Two consequences worth stating, because both are load-bearing:

- **The atlas cache key is the pixel size.** A scale change is a *different*
  request (18 points at 1.5× is a 27 px request), so the glyphs are rasterized and
  hinted for the grid they actually land on rather than magnified. The 1/8-px
  quantization in `docs/text-rendering.md` §2.2 exists for exactly this.
- **A caller never reads a window.** The engine pushes the scale from the
  platform once per frame, before any hook that submits text. `1.0` is the
  default, so a device-free caller and every test are unaffected.

## 4. What is deliberately not scaled

- **`LayoutOptions::max_width == 0`.** "No wrapping" is the absence of a distance,
  not a distance of zero. Scaling it would turn every unwrapped request into a
  request wrapped at width 0.
- **World-space text.** It is size-independent by construction — a component's
  `world_height` is scene units, and the MSDF/Slug backends exist so no screen
  density enters (`docs/text-rendering.md` §1.2).
- **A non-positive scale.** A platform that cannot answer must not erase the text;
  it falls back to 1.0 rather than collapsing every glyph to zero pixels.

## 5. ImGui

ImGui is already correct in both of its spaces and needs no conversion:

- Its SDL3 backend sets `io.DisplayFramebufferScale` to
  `SDL_GetWindowSizeInPixels() / SDL_GetWindowSize()` each frame. That expression
  is identical in ImGui 1.91.9 and 1.92.9, so **it becomes 1.5 the moment the
  window flag is set** and stays 1.0 until then.
- `ImGui_ImplVulkan_RenderDrawData` applies that scale to the viewport and the
  clip rects, so widgets keep their layout in points and their physical size on
  screen. Mouse events arrive from SDL in points, which is the same space, so
  hit-testing needs nothing.

What ImGui does *not* get for free at 1.91.9 is glyph **density**: a 16 px atlas is
magnified by the framebuffer scale, so ImGui text is correctly sized but soft. The
pre-1.92 idiom is a font baked at `size * scale` with `io.FontGlobalScale = 1 /
scale`. This is only worth doing when the engine actually loads a font —
`ImGuiConfig::font_size` is currently unused and the default atlas is what draws.

## 6. Invariants

Asserted device-free in `tests/core/text_layout_tests.cpp`:

- at scale 1.0 the conversion is the identity;
- at 1.5 a 18 pt request becomes 27 px at 1.5× the origin and 1.5× the wrap width;
- a zero wrap width stays zero at every scale;
- a non-positive scale falls back to 1.0;
- a point size keeps the same fraction of the window at every scale.

The runtime form of the same invariant, for field diagnosis: a window configured
as 1280×720 points on a 1.5× output must report `content_scale == 1.5` and a
swapchain extent of 1920×1080. If the extent equals the configured size on a
scaled output, the window flag is not reaching `SDL_CreateWindow`.

## 7. One convention that was wrong once

`PlatformState::drawable_width/height` are filled from the **logical** resize
event (`SDL_EVENT_WINDOW_RESIZED`), not the pixel size. Before this document the
two were always equal, so the name was never tested; with the high-density flag
they diverge, and the name would be a lie. They are not used to size the
swapchain — `VulkanSwapchain` reads `SDL_GetWindowSizeInPixels` directly — but a
reader who trusts the name would size a pixel-space resource with a point-space
number. Treat the swapchain extent, `frame.render_extent` or
`SDL_GetWindowSizeInPixels` as the pixel size, and the resize event payload as
points.
