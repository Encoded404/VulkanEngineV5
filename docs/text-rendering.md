# Text rendering: decisions and rationale

The engine draws text through one shaping/layout/cache pipeline and two paths
that must not be the same path:

- **Screen-space text** is UI copy at roughly 11–32 px. It is rasterized by
  FreeType with hinting into an 8-bit coverage (A8) page and sampled 1:1.
- **World-space text** is scene content at any size, orientation, distance and
  perspective. It must stay sharp when it is scaled or rotated continuously, so
  it is not rasterized to a fixed-resolution bitmap at all: it is either a
  multi-channel signed distance field (MSDF) sampled from an RGBA8 page, or an
  analytic outline blob decoded by the Slug fragment shader.

The worked example is `examples/text_demo/`; run it twice to see both world
backends (`--text-backend msdf`, the default, and `--text-backend slug`).

## 1. Why the two paths differ

### 1.1 Hinting is the quality lever at UI sizes

At 11 px, an unhinted outline is a blurry approximation of the shape: stems land
between pixels and the whole face looks out of focus. FreeType's hinting snaps
that outline to the pixel grid, and the result is the crispness a UI expects.
The rasterizer therefore exists to run hinting:

- `GlyphHinting::Native` runs the font's own TrueType instructions. It is the
  best result when the font ships them and *nothing at all* when it does not —
  many webfonts, the vendored Lato included, carry no instructions.
- `GlyphHinting::Light` runs the auto-hinter in light mode. It works on any
  outline and only nudges edges instead of force-snapping every stem, so it
  keeps the font's proportions. It is the default (`kDefaultGlyphHinting`)
  because it is never absent and never as distorted as the normal auto-hinter.

Hinting produces a bitmap whose pixels are the answer for exactly one size, so
the screen path is size-dependent by construction. That is fine: it is drawn
1:1 at the size it was rasterized at.

### 1.2 Scale, rotation and perspective are continuous in the world

The same bitmap approach breaks down for world text. A glyph drawn at 100 px
needs a different bitmap from the same glyph at 12 px, and a rotated or
perspective-projected glyph is not an axis-aligned bitmap at all. Generating one
bitmap per (glyph, size) would either blur or cost an unbounded cache.

Both world backends remove the resolution from the problem in different ways:

| | MSDF | Slug (hb-gpu) |
|---|---|---|
| GPU input | an RGBA8 field: rgb = 3-channel MSDF, a = true single-channel SDF | an encoded outline blob |
| Reconstruction | median-of-three distance, converted with the field's texel range | analytic coverage: band decode + polynomial roots at the drawn size |
| Encoded in | field texels at a fixed generation size (default 32 px, range 4) | font design units, no size anywhere |
| Cache footprint | one field per (glyph, quantized field size, range) | one blob per glyph, full stop |
| Best at | small/distant text, where a distance field is cheap and stable | large text, where analytic coverage stays exact |

The MSDF page keeps its alpha channel because the world-text shader consumes it
(the true distance keeps the reconstruction sharp below the field's generation
size); no channel is shipped unused. The Slug blob is the stronger form of
resolution independence — the encoded bytes never mention a size, so one encode
per glyph serves every size, rotation and distance (`GlyphBlobEncoder`'s cache
key deliberately has no size in it).

### 1.3 What the paths share

Everything before rasterization is shared, and deliberately single-sourced:

- one `FontFace` per (resource, face index), whose HarfBuzz font is pinned to
  the face's units-per-em;
- one `ShapingCache` of design-unit runs;
- one `LayoutText` implementation for line breaking (UAX#14 via libunibreak),
  wrapping, alignment and baseline advance;
- one submission entry point for screen text, `TextSystem::SubmitScreenText`.

Metrics come from the same HarfBuzz face that produced the advances, never from
a second source, so wrapped lines and shaped glyphs cannot drift apart.

## 2. The seam

### 2.1 What a glyph store is

A *glyph store* turns `(face, glyph id, size-defining parameters)` into
GPU-fetchable bytes plus the placement information for those bytes. Every store
follows the same shape:

- a bounded, thread-safe LRU keyed on the exact request (a bare hash never
  decides equality — collisions would hand one glyph another glyph's pixels);
- entries handed out as `shared_ptr<const T>`, so a bitmap a caller holds stays
  valid after eviction or `Clear()`, and no caller can mutate a run another
  caller is reading;
- resolution methods (`Rasterize`, `Generate`, `Get`) that also reserve the
  destination in a CPU atlas and return its slot.

There is one deliberate exception to "all stores own an atlas":
`GlyphBlobEncoder` has no atlas, because a Slug blob is addressed by a flat
offset in a storage buffer, not by a page rectangle.

### 2.2 The three stores

| Store | Third-party work | Output | Destination | Key |
|---|---|---|---|---|
| `GlyphRasterizer` | FreeType hinted bitmap | A8 coverage | `GlyphAtlas`, page format A8 | face id, resource version, glyph id, **quantized** pixel size, hinting mode |
| `MsdfGenerator` | HarfBuzz outlines → msdfgen | RGBA8 field | `GlyphAtlas`, page format RGBA8 | face id, resource version, glyph id, **quantized** field size, range bits |
| `GlyphBlobEncoder` | HarfBuzz `hb_gpu_draw_encode` | 8-byte-unit blob | flat buffer offset (`GpuTextBlobBuffer`) | face id, resource version, glyph id |

Sizes are quantized to 1/8 px (`QuantizePixelSize`, `QuantizeFieldSize`). A
DPI-scaled UI asks for continuously varying sizes — a 1.25 scale turns 14 px
into 17.5, an animation can hand over any float — and a cache keyed on the raw
float would hold a separate bitmap for every value that happens to be requested.
Eighth-pixel steps are finer than the hinting grid (whole pixels) and finer than
any display can show.

Both atlas-backed stores share `GlyphAtlas` (a pure, device-free rectangle
allocator: deterministic packing, page reuse, bounded LRU eviction that releases
the slot) and `GlyphAtlasGpu` (which owns one page image per page and uploads
only the dirty rectangles). The store's byte source resolves a whole-page
rewrite: the atlas stores rectangles, not pixels, so a fresh or evicted page is
repainted by asking the store for every live entry's bytes.

### 2.3 Why the backend selector lives on the pass

`TextBackend { Msdf, Slug }` is a constructor argument of `WorldTextPass` and a
field of `RendererConfig`, not a per-`Components::Text` field.

A render pass owns exactly one graphics pipeline. The two backends do not just
select a different fragment shader: they declare different descriptor bindings
(the Slug pass adds the blob storage buffer at set 5 binding 1), different push
constants (camera + atlas size vs camera + viewport, both 80 bytes), and
different per-glyph instance layouts — `WorldTextInstance` is 80 bytes with a uv
rect and a page slot; `SlugTextInstance` is 76 bytes with an em box and a flat
blob offset. A per-component selector would therefore require two pipelines in
one pass and a per-instance branch, or two passes. The pass reports a fixed name
and the pipeline rejects a duplicate registration, so "two world-text passes,
one per backend" is not available to an application either.

The selector is resolved once when the pass is built. The pass exposes
`Backend()`, `AcceptsMsdfRuns()` and `AcceptsSlugRuns()`; a queue call for the
other family is a no-op rather than a second layout silently reinterpreted
through the wrong struct. The MSDF default is preserved by the existing
3-argument constructor, so no existing caller changes.

`examples/text_demo` follows the seam rather than working around it: the CLI
flag becomes `RendererConfig::text_backend`, and each frame's entities are
queued through whichever of `QueueRun` / `QueueSlugRun` the pass accepts.

## 3. Caches and their keys

| Cache | Key | Notes |
|---|---|---|
| `ShapingCache` | face `UniqueId`, resource version, exact text bytes, direction, kerning, ligatures | one entry per string, size-independent |
| `GlyphRasterizer` | face id, resource version, glyph id, quantized pixel size, hinting | bounded LRU; eviction releases the atlas slot |
| `MsdfGenerator` | face id, resource version, glyph id, quantized field size, range bits | range compared as an exact bit pattern: a different range is different bytes |
| `GlyphBlobEncoder` | face id, resource version, glyph id | no size |
| `FtLibraryPool` | `FontFace::UniqueId` per slot | see §3.2 |

### 3.1 Shaped runs are keyed on `FontFace::UniqueId()`

The shaping cache's key is the face's *identity*, never its address. An allocator
can hand a destroyed face's address to a different font; two faces both at
resource version 1 would then compare equal and the cache would quietly render
one font's glyphs with another's advances. `FontFace::UniqueId()` is a
process-unique, never-reused counter, so comparing it closes that hole without
requiring the cache to own the face. The resource version is in the key as well,
so a reloaded payload with a new identity is never hit.

`ShapingCache::InvalidateFace(face_id)` drops exactly the runs shaped with one
face; entries already handed to a caller stay alive because entries are shared
pointers.

### 3.2 The FreeType face pool uses the same identity

FreeType documents that an `FT_Face` is not safe to use concurrently — it
carries the size, transform and glyph slot a load mutates. `FtLibraryPool`
therefore keeps one `FT_Library` per worker slot and caches `FT_Face`s per slot,
and that per-slot cache is keyed on the same `FontFace::UniqueId()` and not on
the sfnt buffer's address.

The reason is identical to §3.1 and was forced by the same class of bug: font
hot reload builds a new face, and the allocator may well place its (different)
sfnt buffer at the address the old one freed. An address-keyed cache would then
hand the new face the previous font's parsed `FT_Face`. Each cached entry also
holds a `shared_ptr` to the `FontFace`, which is what keeps the bytes its
`FT_Face` points into alive for as long as the entry is resident.

### 3.3 Shaping is pinned to units-per-em

`FontFace` pins HarfBuzz's scale to the face's units-per-em, so every advance,
bearing and metric is a design-unit number and nothing in a `ShapedRun` depends
on a pixel size. `LayoutText` multiplies by `ScaleForSize(pixel_size)` once, for
one requested size. The payoff is that one shaped run serves every size, and the
cache holds one entry per *string* rather than one per (string, size).

The same pinning is what makes the Slug encoder trivial: HarfBuzz's draw
callbacks emit design units with a y-up baseline, which is exactly msdfgen's
shape space (`shape.inverseYAxis = false`) and exactly the space the hb-gpu
blob is encoded in.

## 4. Threading model

- The shared `FontFace` is **immutable** after construction. HarfBuzz's
  object-lifecycle calls are thread-safe and an immutable object can be read
  concurrently, so shaping and layout run on worker threads against one shared
  face instead of a per-thread copy.
- FreeType is not thread-safe per face, so rasterization acquires one slot from
  a fixed `FtLibraryPool` and holds it for the whole job (a `Lease`, released on
  destruction so an early return cannot strand it). The pool has one slot per
  worker in the global `ThreadPool` — the most that can rasterize at once
  through `ParallelFor`.
- There is deliberately **no `thread_local` FT_Library**. The global thread pool
  outlives and joins its workers, so a `thread_local` destructor would run
  during engine teardown while the pool may still be running a job, on a thread
  the engine does not own; and an idle worker would keep its library and every
  face it ever parsed alive for the life of that thread, unbounded. An explicit
  pool makes the lifetime the engine's: slots are created up front and destroyed
  in a known order on a known thread.
- Shaping and field generation run **outside** the cache lock (they are the
  expensive part); the lock only guards the map and the atlas. The blob
  encoder's single mutable `hb_gpu_draw_t` is serialized by its own encoder
  mutex, so callers after different glyphs still encode concurrently and a race
  on one key simply encodes twice and adopts the winner's entry.

## 5. The hb-gpu transport

### 5.1 Blobs are 8-byte units in design units

`hb_gpu_draw_encode` returns a blob whose addressable unit is **8 bytes** (four
signed 16-bit values — one RGBA16I texel). Measured against the vendored Lato
face (upem 2000):

| glyph | id | encoded bytes | mod 8 | mod 16 |
|---|---|---|---|---|
| `H` | 15 | 1528 | 0 | 8 |
| `o` | 111 | 2928 | 0 | 0 |
| `.` | 314 | 1880 | 0 | 8 |
| `8` | 1142 | 4680 | 0 | 8 |
| `W` | 41 | 3696 | 0 | 0 |

Lengths are always a multiple of 8 and are *not* always a multiple of 16. The
cost is small: these five glyphs are ~14 KB, so a full Latin set is on the order
of 50–100 KB, and because the blob is in design units that cost is paid once per
glyph and not once per size.

`GlyphBlob::UnitCount()` records the unit, and the device-free suite asserts both
halves of the measurement (all multiples of 8; at least one not a multiple of
16) so the 16-byte stride cannot be reintroduced silently.

### 5.2 The atlas accessor is `int2`, not `int4`

Upstream's HLSL header comment says the caller must declare
`StructuredBuffer<int4> hb_gpu_atlas`. `int4` strides 16 bytes, so
`hb_gpu_atlas[offset]` would mis-index every blob. The committed Slang port
declares the buffer as 8-byte `StructuredBuffer<int2, CDataLayout>` elements and
extends the four signed 16-bit halves into the `int4` the algorithm expects.

The extension is pure 32-bit arithmetic (`(raw.x << 16) >> 16`, `raw.x >> 16`),
so it needs no `StorageBuffer16BitAccess` (which `int16_t4` would require). Sign
extension matters: the algorithm uses biased offsets and band counts that do not
fit an unsigned 16-bit value. `hb_gpu_fetch` is the *only* place the blob buffer
is read, so the port really is a one-function adaptation; the rest of the shared
source is carried over verbatim.

A GPU test reads the raw words and the sign-extended `int4` back from a blob
offset and compares them against the blob's own bytes at a unit that provably
contains a negative half, so the check cannot pass on zeros.

### 5.3 The matrix convention

Upstream's `hb_gpu_dilate` is written in **column-vector** form:

```
float4 clipPos = mul (m, float4 (position, 0.0, 1.0));
```

Every engine shader is **row-vector** (`mul(float4(p, 1), m)`), and the engine's
`expand.slang` documents that it stores the transpose of the math matrix to match
that usage. `hb_gpu_dilate` must therefore not be copied verbatim: the two
multiplies are rewritten as `mul(float4(position, 0.0, 1.0), m)` and the same `m`
is used for the dilation and the emitted clip position. This is not cosmetic — a
transposed dilation only mis-sizes the half-pixel edge expansion, which is
invisible until text looks subtly aliased, so the vertex contract is expressed
internally consistently rather than leaning on a caller-side transpose.

### 5.4 Provenance

The Slug coverage math *is* the algorithm: any "improvement" changes the glyphs.
`tests/core/slug_shader_provenance_tests.cpp` therefore pins FNV-1a hashes of the
three non-empty upstream HLSL sources (shared fragment 9534 B, shared vertex
2286 B, draw fragment 1442 B; the draw-stage vertex source is empty because all
vertex work is the shared `hb_gpu_dilate`). A HarfBuzz bump that changes them
fails with the instruction to re-port. A second test reads the committed
`engine/core/shaders/slug_gpu.slang` and asserts it still contains the 8-byte
accessor, its sign extension and the row-vector dilation — so the guard cannot be
satisfied by the file merely existing.

## 6. Hot reload

`TextSystem::ReloadFont` re-reads the resource and, only when the read and the
container check both succeed, builds a new `FontFace`. A rejected or missing file
is a no-op at every level: the resource keeps its bytes and version, the face
keeps its identity, and no cache is touched.

On success the new face has a **new `UniqueId`**, which makes every cache keyed
on the old identity unreachable. `InvalidateFace` then:

- drops the old face's entries from the `ShapingCache`;
- calls `GlyphRasterizer::Reset()`, which drops the glyph cache *and* every CPU
  atlas page;
- releases every GPU page through the bindless ring drain, frame-gated so a frame
  that already recorded text against a page keeps sampling a live image.

The invalidation is **wholesale** rather than a repaint-in-place because a GPU
page cannot be rewritten while a recorded frame may still sample it. Repainting
the live page images would change what an in-flight frame observes; taking fresh
pages and retiring the old ones frame-gated is the only version that is safe
without stalling the device. The CPU atlas is reset to match, because otherwise
the next upload would believe a page it must repaint is already clean. The
`MsdfGenerator` and `GlyphBlobEncoder` caches are keyed on the face identity and
resource version too, so they need no separate invalidation.

`FontWatcher` collects debounced changed paths on its own thread; `FontReloader`
maps a path back to its registered resource and pumps the reload on the main
thread, where the frame pump can order the page retirement. The watcher is inert
when the platform has no backend, so a shipped build simply never reloads.

## 7. Two conventions that were wrong once

Both of these were real bugs that only a sign- or offset-sensitive assertion
caught, and both are now pinned by tests.

### 7.1 Alignment offsets are positive from the box's left edge

`LayoutText` aligns each line inside a box sized `max(wrap_width, widest_line)`.
`LayoutLine::offset_x` is a positive displacement from the box's left edge:
left-aligned is 0, centred is `(box - line) / 2`, right-aligned is
`box - line`. A line wider than the wrap width (an unbreakable word) makes the
box that line's own width, so right-aligning it yields offset 0 instead of
pushing it out of its own box.

### 7.2 World text is built in a y-up frame

The screen path is y-down because screens are y-down, and the FreeType bitmaps
are stored top-down. The engine's world is **y-up** (the camera's up is
`(0, 1, 0)`), and the world-text builder originally reused the screen frame's
signs, which mirrored every glyph vertically. Nothing caught it: right and up had
the expected lengths, the uv rects matched, the ink landed in the projected
region and the depth test still ordered near from far — a vertical mirror
preserves all magnitudes and flips only signs.

The world builder now works in a y-up local frame: a baseline sits at
`-advance_y`, HarfBuzz's glyph offsets add directly, the field's top edge sits
above the baseline by its magnitude, and the quad's up edge runs downward
(−height) to match the atlas's top-down row order. The test asserts the signs
directly (a capital's field top is above the baseline; the up edge runs
downward), because magnitudes cannot see the bug.

## 8. Tests and harness conventions

- **Device-free by default.** The default `ctest` presets exclude the `gpu`
  label; device-bound tests are registered with
  `setup_test_target(<target> LABELS gpu)` and guard every body with
  `TestSupport::IsGpuDeviceAvailable()`. `test_gpu.cppm` creates only an
  instance and enumerates devices, so the guard costs nothing without a device.
  A test body must never create a device on the default path.
- **Structural assertions over whole-frame hashes.** Text coverage is
  antialiased and driver-sensitive, so a whole-frame golden hash would be a
  brittle definition of correctness. The text suites assert structure instead:
  ink appears inside the projected world region and the far corners stay
  background; Slug and MSDF ink bounding boxes are comparable for the same
  string; doubling the em size doubles the ink box; edges carry fractional
  coverage on both paths (so the antialiasing assertion is not vacuous); and the
  Slug/msdf depth ordering is checked against a control pipeline with depth
  testing disabled so occlusion cannot pass by accident.
- **Library-version-pinned constants.** A few constants are facts about a
  third-party build, not definitions of correctness, and must be updated in the
  same commit as the dependency bump:

  | Pin | Value | Version it belongs to |
  |---|---|---|
  | `kHintedCapitalHash32` (FNV-1a of `H`'s A8 coverage at 32 px) | `0x4CC168E6D13A7883` | FreeType 2.13.3 |
  | `kCapitalFieldHash32` (FNV-1a of `H`'s RGBA8 field at 32 px, range 4) | `0x7D9068DBB13105A6` | msdfgen 1.12.1 |
  | `kSharedFragmentSourceHash` | `0xD861285F4FEA9E6F` | HarfBuzz 14.3.0, HLSL |
  | `kSharedVertexSourceHash` | `0xDF0141F02DE42987` | HarfBuzz 14.3.0, HLSL |
  | `kDrawFragmentSourceHash` | `0x5F5C67FCD6627ECB` | HarfBuzz 14.3.0, HLSL |

  The hb-gpu shader hashes exist to force a re-port, not to validate the port;
  the A8 and MSDF hashes are determinism checks used alongside a structural
  assertion that a different request produces different bytes.
