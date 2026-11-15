# Transparency in real-time rendering

A transparent surface does not hide what is behind it. That difference breaks the main assumption of a rasterizer: that one fragment per pixel can win and the rest can be discarded. A transparent pixel can contain light from many surfaces. The result depends on their order.

This report covers the compositing model, the history, the algorithms, the hardware, and the practice in shipping engines. It ends with the case of this engine.

## 1. The problem is order

A rasterizer processes primitives in the order the application submits them. For opaque geometry, the depth test makes the image independent of that order. The nearest fragment wins, and the others do not change the pixel. The hardware can also reject a fragment before the shader runs, which is why opaque overdraw is cheap.

A transparent fragment cannot replace the pixel. It must combine with it. Fixed-function blending does this with a read-modify-write operation:

```
C_out = C_src * a_src + C_dst * (1 - a_src)
a_out = a_src + a_dst * (1 - a_src)
```

The blend is correct only if the destination already holds the composite of every surface behind the source. That is the painter's algorithm: draw from back to front. The `over` operator is associative, so a renderer can group layers. It is not commutative, so a renderer cannot reorder them. Section 2 gives the required blend factors for the color and alpha channels.

Sorting works when a correct order exists. It does not always exist:

- Two triangles can intersect. No order is correct for every pixel they cover.
- Three surfaces can form a cycle. A overlaps B, B overlaps C, and C overlaps A.
- A mesh can fold through itself. Per-triangle order is then correct only per pixel, not per triangle.
- A draw call can contain many instances. The sort key belongs to the draw, not to the instance.

The last case matters for modern engines. Batching and instancing group geometry to reduce draw calls. That grouping removes the per-object order that a painter's algorithm needs. An engine can sort draw calls or it can batch them, but it cannot do both at full efficiency.

Transparency is also a fill-rate problem. The cost scales with the number of transparent fragments per pixel, not with the number of triangles. A 1920x1080 image has 2,073,600 pixels. Ten transparent layers over the whole screen produce 20.7 million fragments, and each one reads and writes a framebuffer or an accumulation buffer. The same ten layers over a small part of the screen cost almost nothing. Particles and smoke therefore dominate transparency cost in practice.

### 1.1 Four things called alpha

The word alpha carries at least four meanings. Most transparency bugs come from mixing them.

| Meaning | What it describes | Correct treatment |
|---|---|---|
| Coverage | The fraction of a pixel that a surface covers | Sample the surface at several points, or use a stochastic mask |
| Transmittance | The fraction of light that passes through a medium | Composite in depth order, or use an OIT method |
| Filter weight | The alpha channel of a texture, used to cut out detail | Premultiply, and filter in linear light |
| Binary mask | A yes/no cutout, such as a leaf or a fence | Alpha test or alpha-to-coverage |

Coverage is a geometric property. It is binary at each sample. Transmittance is an optical property. It is continuous and can depend on wavelength. A leaf is mostly coverage. A pane of colored glass is mostly transmittance. A puff of smoke is both: it has coverage at the particle level and transmittance through the volume.

The renderer uses one alpha value for all four because the blend unit has one alpha channel. The rest of this report shows where that compromise holds and where it fails.

## 2. The compositing model

The standard model is Porter and Duff's `over` operator, published in 1984. It treats an image as a set of colored pixels with an associated alpha. The operator composites a source image over a destination image. For straight, non-premultiplied alpha:

```
C_out = (C_src * a_src + C_dst * a_dst * (1 - a_src)) / a_out
a_out = a_src + a_dst * (1 - a_src)
```

For premultiplied alpha, the color channels already include the alpha:

```
C'_out = C'_src + C'_dst * (1 - a_src)
a_out = a_src + a_dst * (1 - a_src)
```

Premultiplied alpha is the better representation for filtering (Forsyth). Bilinear interpolation is a weighted sum. If the color channels are not multiplied by alpha, a transparent texel with an undefined color contributes that color to the result. Take a bilinear sample between an opaque red texel and a transparent blue texel. With straight alpha, the filtered color is purple `(0.5, 0, 0.5)` and the alpha is 0.5. The blue contributes half of its value to a pixel where it should contribute nothing. With premultiplied alpha, the transparent texel stores `(0, 0, 0, 0)`, and the filtered value is `(0.5, 0, 0, 0.5)`. The fringe is gone.

The same rule applies to mipmaps. A mip chain generated from straight-alpha data averages colors that were never visible. Generate the chain from premultiplied data, or dilate the color into the transparent region before downsampling.

Premultiplication is not always the right import choice. An alpha-tested texture uses alpha as a binary mask. Premultiplying it turns the color in the cut-out region black, and bilinear filtering then bleeds black into the silhouette. Keep a straight copy for cutouts, or dilate the color without changing the alpha (Unity, "Edge Padding").

Premultiplied alpha also makes the blend state simpler. The source factor becomes `ONE` instead of `SRC_ALPHA`. The destination factor stays `ONE_MINUS_SRC_ALPHA`. UI and particle systems therefore often store premultiplied textures.

The alpha channel needs its own factors. If the color channels use `SRC_ALPHA` and the alpha channel does too, the result is:

```
a_out = a_src^2 + a_dst * (1 - a_src)
```

That is not the `over` alpha. The alpha channel should use `ONE` and `ONE_MINUS_SRC_ALPHA` even when the color channels use `SRC_ALPHA`. A pipeline that uses one factor pair for all four channels is wrong whenever the source alpha is below 1.

### 2.1 Alpha is not opacity

A surface with alpha 0.5 does not necessarily transmit half of the light. The alpha blend mixes two colors:

```
C = 0.5 * C_front + 0.5 * C_back
```

A physical medium absorbs light along a path. The transmitted radiance follows the Beer-Lambert law:

```
T = exp(-integral(sigma_t(x) dx))
```

The integral runs along the view ray. `sigma_t` is the extinction coefficient, and it can vary with wavelength. A red filter absorbs green and blue, so its transmittance is a color, not a scalar. A single alpha channel cannot represent that. It can only approximate it with a gray filter.

The scalar alpha that reproduces a colored filter is not `1 - T` in general. For a neutral absorber, the equivalent `over` alpha is `1 - T`. For a colored filter, the equivalent source is per channel. That needs per-channel blend factors or a different representation, such as a multiplication blend or dual-source blending.

A physically based glass shader also needs Fresnel. The reflectance of a dielectric rises to 1 at grazing angles. A constant alpha cannot show that. Schlick's approximation is:

```
F = F0 + (1 - F0) * (1 - cos(theta))^5
```

For a glass-air interface, `F0` is about 0.04. A renderer that uses a constant 0.2 alpha for glass will look wrong at every angle except one. The reflected part is `F`, and the transmitted part is `1 - F`, attenuated along the refracted path. Roughness belongs in the refraction lobe, not in the alpha value.

### 2.2 Depth and the depth buffer

The depth buffer stores one depth per pixel. It is the reason opaque rendering works without sorting. It is also the reason transparent rendering needs a separate path: a transparent surface must not write depth, because a nearer transparent surface would then hide the farther one, and the blend would lose the layers behind it.

The standard compromise is:

- Test transparent fragments against the opaque depth buffer.
- Do not write depth for transparent fragments.
- Draw transparent fragments in a known order.

The first two rules are cheap and correct for occlusion by opaque geometry. The third rule is the hard part. Every technique in this report is a different answer to it.

## 3. How early systems handled it

### 3.1 Two-dimensional hardware and sprite priorities

Before 3D hardware, transparency was a 2D compositing problem. Arcade boards and home computers drew sprites and backgrounds into a frame buffer with a blitter. Each sprite had a priority. The hardware drew the sprites in priority order, and the highest-priority opaque pixel won. Transparency was a color key: one palette entry meant "do not draw this pixel". The result was binary. There was no partial coverage and no blending.

The painter's algorithm was explicit. The application supplied the order. A sprite that needed to appear behind another sprite had a lower priority. This worked because 2D scenes have a natural layer order: background, playfield, characters, UI. It did not generalize to 3D.

### 3.2 PlayStation and Saturn: ordering tables and command order

The PlayStation and the Saturn had no z-buffer. They had a display list and a depth sort.

The PlayStation GPU had no depth buffer (Copetti). Its framebuffer was 15-bit RGB plus one mask bit, and the hardware was "totally unaware of Z coordinates" (Nocash). The CPU built an ordering table: an array indexed by depth, with each entry pointing to a command list. The GTE produced an average Z for each primitive with `AVSZ3` or `AVSZ4`. The CPU linked the packet into the table at that index, and DMA2 walked the table.

A full table with 16-bit depth indices needs 256 KB, which was a large part of the 2 MB of main RAM. Games shifted the index down and traded depth resolution for memory. Packets with the same index were inserted so that the newer packet ran first and appeared behind. The order then came from insertion, not from geometry.

Semi-transparency had four fixed modes, selected per primitive by `GP0(E1h)` bits 5 and 6:

```
mode 0: B/2 + F/2
mode 1: B + F
mode 2: B - F
mode 3: B + F/4
```

There was no per-vertex alpha. A texture bit marked texels as semi-transparent, and the primitive had to use a semi-transparent command. One average Z per primitive means a large floor and a small object on it can share an index. Intersecting polygons cannot be resolved at all.

The Sega Saturn used a similar model. Its VDP1 read a command table and drew parts in table order. The manual says the first parts are farthest and the last are nearest. There was no z-buffer. Color calculation was per part: replace, shadow (halve the framebuffer), half luminance, half translucent (`original/2 + base/2`), and Gouraud combinations. It worked only for RGB-code pixels (Sega, VDP1 User's Manual).

VDP1 blended with its own framebuffer, not with VDP2 backgrounds. A translucent sprite over a scrolling background needed color-bank codes plus VDP2 color calculation and priority. The granularity was the command or part, so a multi-part character and an intersecting sprite could only be ordered by table placement.

This era established a pattern that survived for a decade: sort primitives on the CPU, then blend in that order. The pattern assumes that a valid order exists and that the number of transparent primitives is small. Both assumptions held for the content of the time.

### 3.3 Nintendo 64: a depth buffer and a blender

The Nintendo 64 RDP added a depth buffer and a programmable blender. A primitive could test and write depth, and it could blend with the framebuffer using source and destination factors. It also had an alpha compare test, which gave binary cutouts, and a coverage mode used for anti-aliasing.

The pipeline did depth compare and update, alpha compare, coverage rejection, then blend (N64brew). The hardware still did no sorting. The programming manual required a pass order: opaque, opaque decals, opaque interpenetrating, translucent last. It said that depth-sorting transparent surfaces was "strictly speaking" impossible. Transparent surfaces tested depth but did not write it (Nintendo, N64 Programming Manual).

The N64 also introduced a detail that later engines rediscovered: a "decal" mode that passed the depth test at equal depth and did not write depth. Decals and projected shadows used it to avoid z-fighting. The same idea appears today as depth bias and depth-test-equal decal passes.

The depth buffer solved opaque visibility. It did not solve transparency. Colored transparent surfaces still blended in submission order.

### 3.4 Dreamcast: deferred tiling and an early autosort

The Dreamcast's PowerVR2 rendered in tiles and kept a per-pixel depth value in on-chip memory. The Tile Accelerator binned primitives into per-tile lists, and the image synthesis processor resolved pixels with per-tile depth. It could reject hidden pixels before shading, which made opaque overdraw cheap.

PowerVR2 also sorted translucent polygons by default. KallistiOS exposes `autosort_disabled`; when autosort is off, the application must pre-sort the polygons. Translucent geometry used a separate list (`PVR_LIST_TR_POLY`) after opaque and punch-through geometry (KallistiOS).

The sort operated on polygons, not pixels, so intersecting or large polygons still failed. A game that needed a precise order disabled autosort and submitted back to front. This is an early hardware sorter, not a general solution. It also shows the trade-off that tile-based GPUs still make: the hardware can order work per tile, but blending still costs bandwidth and defeats some hidden-surface removal.

### 3.5 Doom and Build: BSP trees and sprites

Doom had no z-buffer. Its renderer traversed a BSP tree and used a column or span buffer to reject pixels that were already covered. Walls were opaque. Masked midtextures, such as fences and vines, were drawn with a color key. Sprites were collected as "vissprites", sorted by scale, which is distance, and drawn far to near.

`R_DrawSprite` scanned the drawsegs from the end and drew a masked midtexture immediately if it was behind the sprite. Otherwise it clipped the sprite against the wall. Remaining masked midtextures were drawn after all sprites, and the player sprite last (id Software, Doom source). The renderer never computed an alpha blend for these surfaces. They were 1-bit masks.

Doom also had a "fuzz" effect for the spectre enemy. It was not alpha blending. It used a colormap that mapped background colors to darker values, with a dither pattern. The result looked like a translucent blur, but the renderer never read the framebuffer. This is a perceptual effect that imitates transparency without the compositing math.

The Build engine, used by Duke Nukem 3D, had a similar model. `drawrooms` drew sectors and walls. `drawmasks` handled sprites and masked walls. Sprites were shell-sorted by view-space depth, then by absolute Z distance and status number within a group. Masked walls merged with sprites through `spritewallfront`: draw a sprite in front of the nearest wall, otherwise draw the wall and the sprites behind it (Vanilla Duke 3D source). The engine had no general alpha blend.

### 3.6 Quake and GLQuake: water, depth writes, and alpha test

Software Quake had opaque animated water. `r_wateralpha` exists only in GLQuake. GLQuake drew water after the scene with `GL_BLEND` and a per-surface alpha, but the water path did not call `glDepthMask(0)`, so translucent water wrote depth. That could hide a transparent surface drawn later (id Software, Quake source).

GLQuake also disabled `GL_ALPHA_TEST` for the world pass. The common claim that GLQuake alpha-tested every fence is too strong. The checked world code did not uniformly honor per-texel cutouts, even though the sprite and HUD paths used alpha test.

The lesson is state leakage. The transparent pass inherited depth state from the opaque pass, and nobody noticed. The same class of bug appears in modern engines when a transparent pipeline is created from an opaque pipeline and one state bit is missed.

### 3.7 Quake III, Unreal, Source, and Doom 3

Quake III Arena added a shader `sort` key. The key selected a render bin: opaque, decal, see-through, additive, nearest, or a numeric value. It did not sort polygons by depth. The renderer batched by shader, so polygons that shared a sort key and a shader were not individually ordered (id Software, Quake III Arena source). `deformVertexes` changed geometry for particles, beams, and projected shadows. It did not change order.

Unreal and Unreal Tournament used per-actor translucency styles and sorted translucent actors back to front. GoldSrc used render modes such as `kRenderTransTexture` and a render amount. Source added `$translucent` and `$alphatest` and grouped renderables into opaque and translucent lists. The translucent list was sorted per object (Valve, Source SDK 2013). Doom 3 sorted translucent entities per object and drew them after the opaque pass. Alpha-tested materials used discard in the main pass (id Software, Doom 3 source).

None of these engines solved intersections or concave objects. They gave artists a sort priority and asked them to avoid the cases that failed.

### 3.8 The fixed-function convention

By the early 2000s, the standard recipe was fixed:

1. Draw opaque geometry with depth test and depth write.
2. Draw alpha-tested geometry in the opaque pass, with discard.
3. Sort translucent objects by distance, with an artist priority override.
4. Draw translucent geometry with depth test on and depth write off.
5. Accept the errors at intersections and between translucent objects.

This is the convention that `glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)`, `glDepthMask(GL_FALSE)`, and `glAlphaFunc(GL_GREATER, 0.5)` encode. Direct3D had the same controls through `D3DRS_ALPHATESTENABLE`, `D3DRS_ALPHAFUNC`, and `D3DRS_ZWRITEENABLE`. Glide had `grAlphaBlendFunction`, `grAlphaTestFunction`, and `grDepthMask`.

Software renderers used a different trick. They faked alpha with a stipple pattern: a 4x4 ordered dither or a checkerboard discarded pixels. The result needed no framebuffer read and worked in palettized modes. It was visible as a pattern and it was order-dependent, because it was coverage, not transmittance.

### 3.9 What the era established

The era established three ideas that still hold:

- Opaque and transparent geometry need different passes and different depth rules.
- A per-object sort plus an artist override is cheap and works for most content.
- Alpha test is the right tool for coverage, and it keeps the depth buffer valid.

It also established three assumptions that no longer hold:

- The number of transparent objects is small.
- A valid per-object order exists.
- The CPU can afford to sort and submit one draw per transparent object.

Modern content violates all three. The rest of this report covers the methods that replace the per-object sort.

## 4. The classical toolbox

Transparency methods fall into three families. Exact methods store or reconstruct the fragments and composite them in depth order. Approximate methods replace the ordered composite with a commutative operation. Coverage methods do not composite at all. They convert alpha into a sample mask and let the hardware resolve it.

### 4.1 Sorting

Per-object sorting is the cheapest method. The renderer computes a depth for each object, sorts the objects, and draws them back to front. The depth is usually the view-space z of the object's origin or bounding-sphere center. The method is exact for a set of convex, separated objects. It is wrong when an object is large, concave, or intersects another object. Cesium's implementation notes that bounding-sphere keys are wrong for large intersecting geometry and that the order can change with a small view change (Bagnell 2014).

Per-triangle sorting is more accurate and much more expensive. The renderer sorts every triangle by depth, then draws them in that order. The sort can run on the CPU or in a compute shader. It still fails for intersecting triangles, and it can produce popping when the camera moves, because the order changes discontinuously.

A BSP tree gives a correct order for static geometry. The tree partitions space by planes, and a front-to-back traversal yields a back-to-front order for any viewpoint. The order is exact for the polygons in the tree, but it is fixed at build time. Dynamic objects need a separate sort, and the tree cannot handle two polygons that intersect. Splitting polygons can impose an order, at the cost of more geometry, attribute seams, and rebuilds.

Bucket sorting trades accuracy for speed. The renderer quantizes depth into a fixed number of buckets and appends each primitive to a bucket. The buckets are then drawn in order. The PlayStation ordering table is a bucket sort with one bucket per depth value. A small number of buckets produces fewer state changes and a stable order, at the cost of errors within a bucket. Bucket depth peeling applies the same idea to depth peeling, with 32 buckets per pixel and one fragment per bucket (Liu et al. 2009).

All sorting methods share a failure: they need a total order, and geometry does not always provide one. They also conflict with batching. A sorted list has one draw per object or triangle, and each draw has state changes. A batched list has one draw for many objects, and the order inside the draw is fixed by the vertex or index buffer.

### 4.2 Coverage: alpha test, alpha-to-coverage, dithering, stochastic sampling

Alpha testing discards a fragment when its alpha is below a threshold. The fragment is either fully present or fully absent. The depth buffer stays valid, so alpha-tested geometry can be drawn in the opaque pass and can occlude correctly. The cost is a hard edge. At a distance, a leaf texture becomes a shimmering pattern of pixels.

Alpha-to-coverage uses multisampling. The fragment shader outputs an alpha, and the hardware converts it to a coverage mask over the samples in the pixel. With 4x MSAA, the mask has 5 possible coverage levels. With 8x MSAA, it has 9. The color is the same for every covered sample, so the edge is soft without a per-sample shader. The method needs MSAA, and it works best when the alpha is a coverage value, not a transmittance. It is order-dependent for color, because each fragment still blends with the current pixel value.

Dithered or hashed alpha discards fragments with a screen-space pattern. A Bayer matrix or a hash of the pixel coordinate produces a threshold. The expected fraction of surviving fragments equals alpha. The image has noise instead of a hard edge. With a blue-noise pattern and a temporal filter, the noise is less visible. Without a temporal filter, the pattern is stable but visible as a screen-door effect.

Hashed alpha uses a per-pixel hash, so the threshold is stable from frame to frame. A random threshold changes every frame and produces temporal noise. Wyman and McGuire show that hashed sampling interacts better with temporal anti-aliasing and alpha-to-coverage than a random threshold (Wyman and McGuire 2019).

Stochastic transparency extends the idea to the sample level. Each fragment covers a random subset of the MSAA samples with probability alpha. The expected color is the correct alpha-blended result, and the variance falls as `alpha * (1 - alpha) / S` for `S` samples (Enderton et al. 2010). The basic method needs one pass and fixed memory. The paper's corrected estimator needs several passes, and 8 to 16 samples per pixel were adequate for its scenes. The artifact is noise.

These methods solve coverage. They do not solve transmittance. A stack of five translucent surfaces still needs five layers of blending, and a coverage mask cannot represent that.

### 4.3 Depth peeling

Depth peeling renders the scene several times. The first pass finds the nearest fragment at each pixel. The second pass finds the next nearest, using the depth from the first pass as a lower bound. Each pass peels one layer.

The original formulation by Everitt uses two depth buffers and one color target (Everitt 2001). The depth buffers ping-pong: one holds the previous layer, and the other receives the current layer. A fragment passes the depth test if its depth is greater than the previous layer's depth and less than the current layer's depth. The color target accumulates the layer. After N passes, the renderer composites the layers from back to front.

Depth peeling is exact for N layers. It needs N geometry passes and constant memory if the layers are composited as they are produced (Bavoil and Enderton 2011). The number of passes depends on the maximum depth complexity of the scene, which is the maximum number of transparent fragments that cover one pixel, not the scene depth. A fixed cap of 4 to 8 layers is common. Layers beyond the cap are lost.

Dual depth peeling reduces the pass count. It peels the nearest and the farthest layer in the same pass, using an RG32F min-max depth buffer and two color buffers (Bavoil and Myers 2008). That takes `N/2 + 1` geometry passes for N layers, not `N/2`. The implementation is more complex, and the guarantee is that no fragment is blended twice, not that every layer is captured in order.

Depth peeling is a reference method. It is exact, simple to reason about, and slow. The MBOIT paper measured 123 ms for depth-peeling ground truth at 1024x1024 with 49 million fragments on a GTX 1080 Ti. It is useful for validation and for offline rendering. It is rarely the default in a real-time engine.

### 4.4 K-buffers, linked lists, and per-pixel arrays

A k-buffer keeps k fragments per pixel in one geometry pass. The fragments are stored in a fixed set of render targets, and a resolve pass sorts and composites them. The stencil-routed k-buffer uses the MSAA sample mask to route fragments to different samples, which gives 8 fragments per pixel per pass on the hardware of the time and 16 fragments in two passes (Myers and Bavoil 2007). It is exact if k is at least the depth complexity. Otherwise fragments overflow or are merged, and MSAA must be disabled while the buffer is filled.

A per-pixel linked list stores an arbitrary number of fragments. Each pixel has a head pointer, and each fragment allocates a node with color, depth, and a next pointer. Carpenter's A-buffer stored a list of fragments per pixel in software (Carpenter 1984). Yang et al. build the list with a 64-bit atomic exchange in one geometry pass, then traverse and sort the list in a resolve pass (Yang et al. 2010). The method is exact if every fragment is stored.

The cost is memory. A 1920x1080 image needs 8 bytes per pixel for the head pointer, which is 16.6 MB. Each node is typically 16 to 32 bytes. A scene with 8 million transparent fragments needs 128 to 256 MB for the nodes, plus the head array. The worst case is unbounded: a single pixel can receive thousands of fragments. Implementations bound the memory with a fixed node pool and a fragment cap. When the pool is full, new fragments are dropped, and the failure is visible as missing layers.

A fixed per-pixel array avoids the pointer chase and gives a predictable memory bound. Adaptive transparency, by Salvi, Montgomery, and Lefohn, keeps a sorted set of N depth and transmittance nodes per pixel. When the array is full, it removes the node whose removal changes the visibility integral least, and it preserves total transmittance exactly (Salvi et al. 2011). Four nodes need 128 bits per pixel in total. Eight nodes gave stable results. On a Radeon HD 5870 at 1280x720, the method was 5 to 40 times faster than the authors' A-buffer reference for a smoke scene with 15 million fragments. The A-buffer reference took 600 ms.

### 4.5 Weighted blended OIT

Weighted blended OIT (WBOIT), published by McGuire and Bavoil in 2013, avoids sorting entirely. It renders all transparent fragments into two buffers with additive blending:

- An accumulation buffer stores `sum(C_i * a_i * w_i)` in RGB and `sum(a_i * w_i)` in alpha.
- A revealage buffer stores `prod(1 - a_i)`, computed with multiplicative blending.

A resolve pass divides the accumulated color by the accumulated alpha and composites the result over the scene with the revealage as the remaining background weight:

```
C = sum(C_i * a_i * w_i) / sum(a_i * w_i)
a = 1 - prod(1 - a_i)
```

The weight `w_i` is a function of depth and alpha. It gives more influence to fragments that are near the camera and to fragments with high alpha. The paper gives three generic functions. The first is:

```
w(z, a) = a * clamp(10 / (1e-5 + (|z|/5)^2 + (|z|/200)^6), 1e-2, 3e3)
```

where `z` is the camera-space depth. The constants are tuned for a depth range of 0.1 to 500 units and a 16-bit accumulation buffer. A different scene scale needs different constants. A common variant uses `0.03 / (1e-5 + (|z|/200)^4)` instead of the rational polynomial.

WBOIT is order-independent, uses one geometry pass and one full-screen resolve, and needs two render targets: an RGBA16F accumulation buffer and an R16F revealage buffer. That is 10 bytes per pixel, or 20.7 MB at 1080p and 82.9 MB at 4K. The coverage of the background is exact. The color is an approximation: a weighted average of the layer colors.

The paper is explicit about the failure cases. WBOIT fails when the colors of the layers differ strongly with depth, such as a white cloud in front of a dark cloud, and when the medium is colored. It cannot represent per-channel transmittance. It also saturates the 16-bit accumulation buffer after about 20 layers close to the camera. Low contrast is not a failure case; it is where the method looks best.

### 4.6 Moment-based OIT

Moment-based OIT (MBOIT), published by Münstermann et al. in 2018, stores a set of power moments of the transmittance function per pixel. The moments allow the resolve pass to reconstruct the transmittance at any depth, so it can composite the layers in depth order without storing them.

The method renders all transparent fragments into several moment buffers with additive blending. A resolve pass reconstructs the transmittance curve, finds the depth of each layer, and composites front to back. The number of moments controls the accuracy. Four moments need 80 bits per pixel, six need 112 bits, and eight need 160 bits. Four moments leak with complex depth distributions, six is close, and eight is slightly better.

MBOIT needs two geometry passes and a composite pass. The composite pass took about 0.06 ms in the paper. On a GTX 1080 Ti at 1024x1024 with 49 million transparent fragments, six power moments took 12 ms, and three trigonometric moments at 224 bits took 16 ms. Depth-peeling ground truth took 123 ms. The method handles colored media better than WBOIT and has a bounded memory cost. The reconstruction can produce ringing, and it still assumes a one-dimensional transmittance function along the view ray.

### 4.7 Other approximations

Fourier opacity mapping represents transmittance versus depth as a truncated Fourier series. It accumulates coefficients in a shadow-map-like pass and reconstructs `T(z)` in a second pass. Seven or fifteen coefficients are common. It assumes smooth, low-opacity media. Hard blockers produce ringing and smeared shadows (Jansen and Bavoil 2010).

Phenomenological transparency, by McGuire and Mara, uses a single order-independent pass with multiple render targets to model colored transmission, colored shadows, caustics, diffusion, and refraction. It uses 112 bits per pixel and one draw call for all transmissive surfaces. On a GTX 980 at 1920x1080, the OIT pass ranged from 0.04 ms for a door to 19.21 ms for 24 million hair fragments. It is a perceptual model, not a general exact OIT (McGuire and Mara 2017).

Multi-layer alpha blending (MALB) keeps k layers per pixel in one pass. Each layer blends the fragments assigned to it, and the resolve blends the layers. It costs 80 bits per layer, or 160 bits for two layers and 320 bits for four. It is bounded memory, but layer assignment causes discontinuities and wrong mutual occlusion for dense particles (Salvi and Vaidyanathan 2014).

Hybrid transparency composites a few core layers accurately and approximates the rest with a weighted tail. It is bounded memory and often the lowest-memory option that keeps a few layers exact, but the tail is still approximate (Maule et al. 2013).

### 4.8 Cost comparison

The following table gives the cost of each method at 1920x1080, excluding geometry cost. The numbers are for the buffers only. The geometry cost is one pass over the transparent geometry for every method except depth peeling, which repeats it.

| Method | Correct order | Geometry passes | Buffer memory at 1080p | Main failure |
|---|---|---|---|---|
| Per-object sort | Approximate | 1 | 0 | Intersections, large objects |
| Per-triangle sort | Approximate | 1 | 0 | Intersections, popping |
| Alpha test | Coverage only | 1 | 0 | Hard edges, shimmer |
| Alpha-to-coverage | Coverage only | 1 | 4x or 8x MSAA | Needs MSAA, limited levels |
| Hashed alpha | Stochastic | 1 | 0 | Noise, needs temporal filter |
| Depth peeling (8 layers) | Exact to 8 | 8 | 8 color targets + 2 depth targets | Bandwidth, pass count |
| Linked list | Exact to pool | 1 + resolve | 16.6 MB heads + nodes | Unbounded memory, atomics |
| WBOIT | Approximate | 1 + resolve | 20.7 MB | Colored layers, strong depth contrast |
| MBOIT (6 moments) | Approximate | 2 + resolve | 29 MB | Ringing, resolve cost |
| Adaptive (4 layers) | Exact to 4 | 1 + resolve | 33 MB | Fixed layer cap |

The memory numbers assume FP16 color, an R16F revealage buffer, and 32-bit depth. A real implementation can use fewer bits for some buffers. The point is the order of magnitude: WBOIT and adaptive arrays cost tens of megabytes, linked lists cost hundreds, and depth peeling costs bandwidth rather than memory.

## 5. What production engines do

### 5.1 Unreal Engine

Unreal Engine sorts translucent objects per object by default. The project settings define the sort policy: distance from the camera to the bounding-sphere center (the default), projected Z, or distance along an axis. The policy requires Separate Translucency, which renders translucency to a separate target and composites it after depth of field. Each primitive has a translucency sort priority that overrides the distance order.

The material option "Sort Triangles" sorts triangles inside one mesh. It is not a global order, and it cannot order two meshes or two particles. Epic documents the sort key, not a general sorter (Epic Games, "Rendering Settings").

Unreal does not ship a general OIT path. Lit translucency uses a cascaded volume texture around the view frustum. The default dimension is 64, and doubling it increases the lighting cost of the volume by a factor of 8. Lit translucent surfaces are missing direct specular, and they get their direct lighting from the volume at lower resolution (Epic Games, "Lit Translucency").

`AlphaComposite` and `AlphaHoldout` are blend modes, not OIT. `AlphaComposite` multiplies the scene color by the inverse of the material opacity. `AlphaHoldout` punches holes in translucency behind it (Epic Games, "Material Blend Modes").

Single Layer Water is also not WBOIT. It is a custom pass with an Opaque or Masked blend mode. It writes the G-buffer and depth, runs after the base pass and deferred lighting but before regular translucency, and composites screen-space reflections in a full-screen pass. On low-end hardware it falls back to a simple translucent material (Epic Games, "Single Layer Water").

### 5.2 Unity

Unity's Universal Render Pipeline renders transparent materials in a separate pass after opaque objects. Materials with a lower sorting priority render first, then distance. The opaque texture is a snapshot of the scene before transparents, so transparents do not refract each other (Unity, "URP Lit Shader").

URP's Lit shader has an alpha-to-mask render state, but the property is hidden from the material inspector. Alpha-to-coverage is available to shaders and custom passes, not as a normal material toggle.

HDRP replaces the render queue with two priorities: a material sorting priority from -50 to 50, then a renderer priority. A lower material priority renders first, even if the mesh is farther away. Enabling Depth Write on the material makes HDRP ignore the sort priority (Unity, "Renderer and Material Priority").

HDRP also has a Transparent Depth Prepass. It adds polygons from the transparent surface to the depth buffer before the lighting pass, which improves sorting and lets the lighting pass reject hidden pixels. A matching postpass and a Depth Offset option exist. "Back Then Front Rendering" issues two draw calls for the front and back faces (Unity, "Transparent Depth Prepass"). None of these is OIT.

### 5.3 Godot

Godot 4 sorts transparent objects back to front by the `Node3D` origin, not by the vertex position. Render Priority and Sorting Offset are the overrides. The manual states that Godot does not provide order-independent transparency (Godot, "3D Rendering Limitations").

Godot has five transparency modes:

- Alpha: the slowest mode, and it disables shadow casting.
- Alpha Scissor: opaque parts render in the depth prepass and cast shadows.
- Alpha Hash: a spatially deterministic threshold, recommended for hair.
- Depth Pre-Pass: discards alpha below 0.99 in the prepass and below 0.1 in the shadow pass.
- Disabled.

The mode names and costs are documented in the `BaseMaterial3D` class reference (Godot, `BaseMaterial3D`).

### 5.4 Filament, Blender, three.js, and Bevy

Filament has opaque, transparent, fade, add, masked, multiply, screen, and custom blend modes. Masked materials enable alpha-to-coverage. Transparent materials can use one pass or two passes for front and back faces. The documentation says that none of these methods can accurately render concave geometry, but that they are often good enough (Google, "Filament Materials"). Screen-space refraction samples the color buffer. Transparent shadows use a dither pattern.

Blender's EEVEE Next splits materials by render method: Dithered or Blended. The manual states that EEVEE does not support per-fragment or per-triangle sorting. Only per-object sorting is available, and it is automatic. Sorting can be disabled when a scene has thousands of objects (Blender, "EEVEE Material Settings").

three.js sorts transparent objects when `sortObjects` is true, which is the default. The transparent comparator uses group order, then render order, then depth from far to near. The opaque comparator uses material id and then depth. Transmission materials render the opaque scene into a separate target and sample it with a refraction offset (three.js, `WebGLRenderer`). The WebGL renderer has no WBOIT addon. WBOIT exists only as a WebGPU/TSL OIT pass node (three.js, `webgpu_oit`).

Bevy splits 3D work into `Opaque3d`, `AlphaMask3d`, and `Transparent3d`. Opaque and alpha-mask phases are binned and unsorted. `Transparent3d` is sorted by distance. The material `AlphaMode` enum has opaque, mask, blend, premultiplied, alpha-to-coverage, add, and multiply (Bevy, `AlphaMode`). There is no built-in OIT phase.

### 5.5 id Tech, Frostbite, Decima, and Naughty Dog

id Tech 6 uses a hybrid forward and deferred renderer. The SIGGRAPH 2016 talk notes that particles can produce heavy overdraw, and the engine relies on alpha-tested foliage and temporal anti-aliasing to control the cost (id Software, "id Tech 6"). It does not document a general OIT path.

Public information on Frostbite, Decima, and Naughty Dog is thin. Frostbite's public talks cover volumetric rendering, not transparency sorting (Hillaire 2015). Decima's public talks cover visibility and culling (Guerrilla Games 2017). No primary source documents a shipped OIT pipeline in these engines. The claim that Frostbite ships stochastic transparency is widely repeated, but the technique is Wyman's research, and no EA primary source surfaced. Treat that claim as unverified.

### 5.6 Mobile and tile-based GPUs

Mobile GPUs from PowerVR, Arm, and Qualcomm use tile-based deferred rendering. The GPU bins geometry into tiles, then shades each tile with the geometry that covers it. The depth test runs in tile memory, so opaque overdraw costs little bandwidth.

Blending changes that. A blended fragment must read the current tile value, combine, and write it back. The GPU cannot reorder blended fragments, and it cannot use the same hidden-surface removal that works for opaque geometry. Imagination states that enabling alpha blending eliminates the benefits of deferred rendering, because the hardware can no longer decide visibility before shading (Imagination, "Do Not Use Alpha Blend Unnecessarily").

Tile-based GPUs offer two features that help. Pixel local storage on Mali keeps a small per-pixel buffer in tile memory, which a shader can use for a linked list or an accumulation buffer without going to system memory (Arm, "Pixel Local Storage"). Programmable blending or framebuffer fetch lets a shader read the current framebuffer value. Both are vendor-specific and need a fallback.

A Vulkan subpass can read the previous subpass through an input attachment, so the data stays in tile memory. The limitation is that barriers can only be inserted between draws, not inside one. `VK_EXT_rasterization_order_attachment_access` adds per-fragment ordering for attachment reads, and dynamic rendering local read and shader tile image extensions provide similar access (Arm, "Framebuffer Fetch in Vulkan").

## 6. Hardware and API support

### 6.1 Rasterizer order views and fragment shader interlock

A rasterizer order view (ROV) is a resource that the GPU accesses in primitive order. Two fragments that cover the same pixel are processed in the order the rasterizer produces them, even if they come from different triangles. The guarantee applies to invocations from the same draw call. It does not order different draw calls (Microsoft, "Rasterizer-Ordered Views").

Direct3D 11.3 introduced ROVs. ROV support is mandatory at feature level 12_1 and optional at 12_0 and 11_1 (Microsoft, "Feature Levels"). Intel's AOIT sample uses ROVs to build a fixed-node visibility function without a linked list, and GRID 2 and GRID Autosport used Intel's earlier pixel synchronization extension (Intel, "AOIT-Update"). AMD implements interlocks with a primitive order packer, or POPS. The wave waits for a packer handshake before it enters the ordered section, and it must make its accesses coherent through L2. Pixel interlock with MSAA can be much slower than sample interlock, and GFX10.3 forces 1x1 shading when POPS is on (Mesa, "POPS").

Vulkan's equivalent is `VK_EXT_fragment_shader_interlock`, ratified in 2019 (Khronos, "VK_EXT_fragment_shader_interlock"). A shader wraps a critical section in `beginInvocationInterlock` and `endInvocationInterlock`, and the GPU guarantees that only one fragment invocation per pixel is inside the section at a time. About 26 percent of devices in the Vulkan hardware database expose pixel interlock, and about 9 percent expose shading-rate interlock (Vulkan Hardware Database).

Both features make a per-pixel linked list or array safe without a spin lock. They do not make blending order-independent. A shader still has to store the fragments and resolve them later. The critical section serializes fragments, so it can be slower than atomics on a GPU with a fast 64-bit atomic.

A weaker primitive is an atomic min or max on an R32_UINT depth surface. It gives nearest-depth visibility without a depth attachment, but it gives no blend order, it does not imply a memory fence, and it loses hardware early-Z (Microsoft, "InterlockedMax"). It is a visibility pass, not OIT.

### 6.2 Multisampling, coverage, and variable-rate shading

MSAA stores several depth and color samples per pixel. The rasterizer computes coverage at each sample, and the fragment shader runs once per covered pixel, or once per sample if sample shading is enabled. The resolve pass averages the samples.

MSAA gives alpha-to-coverage its coverage levels. It also gives a correct result for alpha-tested geometry at the edges, because the coverage mask is computed per sample. The cost is memory and bandwidth: 4x MSAA multiplies the color and depth storage by 4, and the resolve pass reads all samples.

Variable-rate shading lets the renderer shade some regions at a lower rate. It reduces cost for smooth areas, but it can make alpha-tested edges blocky. Depth, stencil, and coverage are still computed at full sample resolution. VRS is a rate trade, not an ordering fix, and a renderer should keep full rate on foliage and on any surface with high-frequency alpha (Microsoft, "Variable-Rate Shading").

Conservative rasterization solves the opposite problem: a thin triangle can miss a pixel center and disappear. Conservative tier 1 guarantees that any pixel touched by the triangle is shaded. Tier 3 adds an inner coverage value. It is useful for thin geometry and for some OIT schemes, at the cost of overdraw.

### 6.3 Tile memory and attachment access

On a tile-based GPU, a render pass can have several subpasses. A subpass can read the color or depth produced by an earlier subpass through an input attachment, without writing the data to system memory. This is the tile-memory equivalent of framebuffer fetch.

The feature is useful for OIT on mobile. A first subpass can render transparent fragments into an accumulation buffer in tile memory, and a second subpass can resolve it. The data never leaves the tile. The limitation is that the buffers must fit in the tile working set, which is small compared with a full-screen target.

Vulkan exposes this through render passes and input attachments. `VK_EXT_rasterization_order_attachment_access` adds per-fragment ordering for reads and writes of attachments, which makes some OIT schemes possible without a separate accumulation buffer (Khronos, "VK_EXT_rasterization_order_attachment_access"). `VK_KHR_dynamic_rendering_local_read` and `VK_EXT_shader_tile_image` provide similar access with dynamic rendering. Qualcomm's `VK_QCOM_tile_shading` exposes per-tile draws and tile attachments (Khronos, "VK_QCOM_tile_shading"). Depth and G-buffer images can be marked as transient and lazily allocated so they never reach DRAM (Imagination, "On-Chip Memory").

### 6.4 Ray tracing

Ray tracing handles transparency naturally. A ray that hits a transparent surface can continue through it with a weight, and the path terminates when it reaches an opaque surface or leaves the scene. Alpha-tested geometry uses an any-hit shader that discards the hit when the texture alpha is below the threshold.

Any-hit shaders are invoked even for hits that are trivially opaque or transparent. DirectX Raytracing 1.2 adds opacity micromaps, which attach one or two bits per micro-triangle and let traversal skip the any-hit shader in uniform regions (Microsoft, "Opacity Micromaps"). All NVIDIA RT-capable hardware supports them, and Remedy reported about one third lower ray-tracing cost in Alan Wake 2 from opacity micromaps plus shader execution reordering.

For transparency proper, alpha as coverage has returned in research. A 2025 EGSR paper treats alpha as coverage, accumulates over frames with temporal anti-aliasing, shades one fragment per pixel, and uses early ray termination. The method is cheaper than conventional OIT, but it is amortized over time (Spatio-Temporal Dithering 2025).

Ray-traced transparency is shipping in games that already have a ray-traced pipeline, usually for reflections, shadows, and path-traced modes. It is not a general replacement for raster transparency, because the cost is high and the noise is hard to denoise at low sample counts.

### 6.5 Gaussian splatting

A Gaussian splat is a transparent primitive with a 3D position, a covariance, and a color. The renderer projects each splat to a 2D ellipse, bins it into a 16x16 tile, and sorts by a combined tile and depth key (Kerbl et al. 2023). The sort key is the view-space z of the splat center, which is a global order onto spherical shells. It produces popping under rotation.

StopThePop shows that a full per-pixel sort costs more than 100 times a tile sort, while a hierarchical per-pixel resort costs about 4 percent (Radl et al. 2024). 2D Gaussian splatting replaces volumes with oriented disks and ray-splat intersection for view-consistent geometry (2DGS 2024). EVER drops alpha compositing for exact ray-traced volume rendering, which removes popping and view-dependent density at about 30 frames per second at 720p on an RTX 4090 (EVER 2024).

Gaussian splatting is a new content type, not a replacement for triangle transparency. It is relevant because it shows the same problem in a different form: a set of transparent primitives with no global order, resolved by a per-tile sort.

### 6.6 Recent research

Neural Moment Transparency learns per-fragment transmittance and composes it with moment statistics in the fragment shader. It reports better accuracy than prior OIT at high depth complexity (Tsopouridis et al. 2024). NeuralTO reconstructs and synthesizes translucent objects with a neural representation (SIGGRAPH 2024).

Moment-based methods have also been applied to Gaussian splats. A 2025 paper computes per-ray transmittance from moments and avoids per-pixel sorting (Moment-Based 3DGS 2025).

The binding constraint in all of this work is bandwidth and working-set size, not arithmetic. Fixed-size node arrays exist to avoid list storage and overflow. Sort-free splatting trades memory for consistency. On the ray-tracing side, the trend is BVH-side opacity metadata plus coherence reordering, not more shading.

## 7. Color, light, and perception

### 7.1 Blend in linear light

The `over` operator is a linear operation on radiance. It is correct only if the color values are linear. If the renderer blends sRGB-encoded values, the result is wrong: dark regions get too much weight and bright regions too little.

The error is large. A 50 percent blend of white over black in sRGB space gives 128, which is about 22 percent of the linear radiance. The correct linear result, 0.5, encodes to about 186. The sRGB blend is about 2.3 times too dark (Novak 2016).

A framebuffer with an sRGB format decodes to linear before blending and encodes after. That is correct, and it is free (Gritz and d'Eon). A framebuffer with a UNORM format does not decode. A shader that writes sRGB-encoded values into a UNORM target and enables blending produces the wrong result, even if the textures are tagged as sRGB.

Alpha is never gamma encoded. The sRGB framebuffer extension leaves alpha linear, because alpha is coverage or transmittance, not radiance (EXT_framebuffer_sRGB). A shader that applies the sRGB transfer function to alpha is wrong.

The color space of the target matters more than the color space of the textures. The renderer should composite in a linear format, then encode once at the end. This engine already does that: its scene target is FP16 linear, and the output pass encodes to the display.

### 7.2 Tone mapping and compositing order

Tone mapping is a non-linear operation. Compositing before tone mapping and compositing after tone mapping give different results. The correct order depends on what the alpha represents.

For a physical surface, the radiance of the layers should be composited first, then tone mapped. The `over` operator is a linear combination. It commutes with any linear operation but not with a non-linear tone curve. If each layer is tone mapped before compositing, a bright layer loses energy and the blend is wrong.

The same argument applies to premultiplied color. Tone mapping does not commute with multiplication by alpha:
```
tonemap(a * C) != a * tonemap(C)
```

Do not tone map a premultiplied layer before compositing it. Composite scene-referred radiance, then tone map once, then apply the output transfer function.

For UI and text, alpha usually represents coverage. The color is often authored in display space. Compositing coverage in linear light is correct if the color is converted to linear first. This engine decodes text colors to linear, blends in the linear scene target, and encodes at output. That is correct for coverage, as long as the text color is treated as a linear radiance, not as a display value.

The practical rule: decide what alpha means for the content, then composite in the space where that meaning is linear. Do not mix the two in one buffer.

### 7.3 The perceptual cues of transparency

The eye uses several cues to judge transparency:

- The background is visible through the surface, with reduced contrast.
- The surface has a specular reflection that changes with the viewing angle.
- The color of the transmitted light is filtered by the medium.
- The edges of the surface refract the background.

A constant alpha blend provides only the first cue. It reduces contrast uniformly and does not change with the angle. Alpha-blended glass therefore looks like plastic. A convincing glass shader needs Fresnel, refraction, and a reflection term. The blend is only the final composite.

Filament's material model makes the distinction explicit. Its `transparent` mode applies alpha to diffuse lighting only. Its `fade` mode fades the specular term too, because a fading object must lose its reflection as well as its diffuse color (Google, "Filament Materials").

Unreal's Substrate model separates coverage from transmittance. Coverage is a binary or partial mask. Transmittance is a colored function of thickness. Substrate supports colored transmittance through dual-source color blending, with gray transmittance as the cheap fallback (Epic Games, "Substrate Materials"). That is the direction a physically based transparency path needs to go. A single scalar alpha cannot represent a red filter over a blue background.

Smoke and fog are different. They have no sharp surface, no Fresnel, and no refraction. A constant alpha or a volume rendering integrates the medium along the ray. The visual cue is the loss of contrast with distance, which a depth-weighted blend can approximate.

### 7.4 Temporal artifacts

Stochastic methods trade spatial noise for temporal noise. A single frame of hashed alpha looks like a screen-door pattern. With temporal anti-aliasing, the pattern changes every frame and the filter averages it. The result is smooth in static areas and smeared in motion.

The smear is the main cost. A moving object with hashed alpha leaves a trail of noisy samples. A temporal filter with motion vectors can compensate, but the motion vectors must be correct for the transparent surface. A surface with refraction or distortion has no simple motion vector.

A stable alternative is a fixed dither pattern. It does not flicker, but it is visible as a pattern, especially on a high-contrast background. Blue noise makes the pattern less objectionable than a Bayer matrix. A spatiotemporal blue-noise mask pushes the error to high frequencies and varies it over time, which is better for a temporal filter (Wolfe et al. 2021). The choice depends on the content and on whether a temporal filter is available.

### 7.5 Test scenes

A renderer should test transparency with a simple scene before it tests a complex one:

- A red filter in front of a blue filter, to test colored transmittance.
- A glass sphere in front of a checkerboard, to test Fresnel and refraction.
- Two intersecting transparent planes, to test the limits of sorting.
- A stack of ten low-alpha quads, to test depth weighting and saturation.
- A moving transparent object, to test temporal stability.

If the result is wrong in the simple scene, it will be wrong in the complex one.

## 8. Performance

Transparency cost has three parts:

1. Geometry cost. The vertex shader and the rasterizer process every transparent triangle. This cost is proportional to the triangle count and does not depend on the blend method.
2. Fill cost. Every transparent fragment runs the fragment shader and the blend. This cost is proportional to the number of transparent fragments, which is the depth complexity times the covered pixels.
3. Buffer cost. OIT methods read and write extra buffers. This cost is proportional to the resolution and to the number of buffers.

The fill cost usually dominates. A particle system with 1000 large quads can cover the screen many times. The fragment shader may be cheap, but the blend and the bandwidth are not. A renderer that reduces overdraw, for example by culling particles behind opaque geometry or by using a lower-resolution particle buffer, gains more than a renderer that switches from WBOIT to a more accurate method.

The buffer cost matters at high resolution. WBOIT at 4K needs 82.9 MB for the two targets. MBOIT with six moments needs 29 MB at 1080p and 116 MB at 4K. Linked lists need a node pool that can handle the worst case, and the pool size is a guess.

A useful measurement is the number of transparent fragments per pixel. The renderer can count them with a compute pass or with a GPU query. A scene with an average of 2 layers needs a different method from a scene with an average of 20. The average is not enough: the tail matters. A few pixels with 100 layers can force a large node pool or a high depth-peeling cap.

### 8.1 The batching conflict

Sorting and batching pull in opposite directions. A sorted draw list has one draw per object, or per triangle, and each draw has state changes. A batched draw list has one draw for many objects, and the order inside the draw is fixed by the vertex or index buffer.

A renderer can resolve the conflict in three ways:

- Sort within a batch. The batch is small enough that the order inside it does not matter, or the batch is built in depth order on the CPU. This is the classic per-object sort with instancing disabled for transparent objects.
- Sort the batches. The batch key includes a depth bucket. Objects in the same bucket are batched, and the buckets are drawn in order. The error is bounded by the bucket size. This is the PlayStation ordering table on a modern GPU.
- Do not sort. Use an OIT method that tolerates arbitrary order. The cost is the extra buffers and the approximate result.

Most engines use the first option for large transparent surfaces and the third for particles. The second option is rare because it needs a depth bucket in the draw key and a stable sort, but it is a good compromise for foliage and small props.

### 8.2 Measuring

A transparency benchmark should report:

- Frame time with transparent geometry disabled, with it enabled, and with the OIT resolve enabled.
- The number of transparent fragments and the maximum per pixel.
- The memory used by the OIT buffers.
- Image error against a reference, such as a path-traced render or a high-layer depth peel.

PSNR is mean-squared error and does not track perception. FLIP approximates the difference a human sees when alternating between two images, and it beat older metrics in its user study (Andersson et al. 2020). Report mean FLIP alongside PSNR, freeze the camera and animation for spatial comparisons, and evaluate sequences for temporal artifacts. Hold resolution, sample budget, temporal history, exposure, and tone mapping fixed, or the comparison is not meaningful.

## 9. Choosing a method

The choice depends on the content, the hardware, and the acceptable error. The following table is a starting point.

| Content | First choice | Why | Avoid |
|---|---|---|---|
| Foliage, leaves, fences | Alpha test, then alpha-to-coverage or hashed alpha | Coverage, not transmittance; depth writes stay valid | Sorted blend, WBOIT |
| Glass, windows | Sorted blend with Fresnel and refraction | Needs correct color and reflection | WBOIT for colored glass |
| Water | Separate water pass with depth-based absorption | Needs refraction, reflection, and depth | A single alpha blend |
| Smoke, fog, particles | WBOIT or sorted particles | Many low-alpha layers; exact order is less visible | Depth peeling, linked lists |
| Hair | Alpha test or hashed alpha with TAA | High-frequency coverage | Sorted blend |
| Decals | Alpha blend with depth bias | Coplanar with the surface | OIT |
| UI and text | Straight or premultiplied alpha, linear composite | Coverage of display-referred colors | OIT, depth sorting |
| Impostors | Alpha-to-coverage or alpha test | Distant coverage | Per-object sort |

A planning budget for a 60 Hz frame at 1080p on a mid-range GPU is:

- Alpha test or alpha-to-coverage: under 0.5 ms for a full-screen layer of foliage.
- Sorted blend: under 1 ms for a few large transparent surfaces.
- WBOIT: 1 to 2 ms for a full-screen particle layer, including the resolve.
- MBOIT: 3 to 5 ms for the same layer.
- Linked list: 2 to 5 ms, plus the memory.
- Depth peeling with 8 layers: 5 to 10 ms, mostly bandwidth.

These are order-of-magnitude targets, not benchmark results. They depend on the GPU, the resolution, and the fragment shader. The relative order is more reliable than the absolute values. Published measurements vary by more than an order of magnitude because the scenes and resolutions differ.

A renderer that needs exact results for a few layers and approximate results for many layers can combine methods. For example:

1. Draw opaque geometry.
2. Draw alpha-tested geometry in the opaque pass.
3. Draw large glass surfaces with a sorted blend, back to front.
4. Draw particles and smoke into a WBOIT buffer.
5. Resolve WBOIT over the scene.
6. Composite the sorted glass and the WBOIT result in a fixed order.

The fixed order between the two groups is a compromise. It is correct if the glass is in front of the particles or if the particles are in front of the glass, but not both. A renderer can split the particles into "behind glass" and "in front of glass" groups when the glass is a known surface, such as a window.

## 10. Applying this to this engine

This section describes the current state and a sequence of changes. It is not a claim that the changes are implemented.

### 10.1 Current state

- The scene target is `R16G16B16A16Sfloat`, linear, with one sample. The output pass encodes to the display.
- Materials have three blend modes: opaque, cutout, and transparent.
- A transparent material uses source-alpha / one-minus-source-alpha blending. It tests depth with `less-or-equal` and does not write depth.
- Draws are grouped by technique, material, and render state. The collect pass emits one indirect draw per group. The main pass binds the group pipeline and issues the draw.
- There is no depth sort for transparent groups. The draw order is the group order, which is not related to depth.
- The depth prepass is enabled per technique, not per material. Transparent groups are not excluded. Their depth goes into the prepass buffer.

The last point has two consequences. By construction, the prepass stores the nearest transparent fragment at each pixel. The main pass then tests every fragment against that depth with `less-or-equal`. A second transparent layer behind the first is rejected, even though it should blend through the first. In addition, opaque geometry behind a transparent surface can be rejected, because the transparent surface wrote depth in the prepass. The transparent surface then blends over the clear color. The path is order-independent, but it is not correct for a scene with a transparent surface in front of opaque geometry.

The engine's depth prepass is useful for opaque geometry: it fills the depth buffer once and lets the main pass run with depth test equal. For transparency, it works against the goal.

### 10.2 A minimal fix

1. Exclude groups whose blend mode is transparent from the depth prepass. The cull shader already reads the technique flags. Add a check on the render-state bits in the draw key, or add a per-group flag.
2. Keep the main pass depth test against the opaque depth buffer. Transparent fragments behind opaque geometry are still rejected. Transparent fragments behind other transparent fragments are not.
3. Accept that the transparent order is the group order. For a first step, that is better than a single layer, but it can still show the wrong order between two transparent materials.

This fix costs no memory and removes the single-layer limitation. It is a prerequisite for every later step. Verify it with two transparent quads at different depths. The far quad must be visible through the near one.

### 10.3 Per-object sorting in the collect path

The collect pass already builds one draw per group. A sorted path needs a depth per submesh, not per group. The engine has the submesh entries and the view-projection matrix on the GPU, so a compute pass can compute a depth for each transparent submesh and write a sort key.

The sort can run in three places:

- CPU. Read back the visible transparent submesh list and sort it. This is simple, but it adds a readback and a frame of latency.
- GPU compute. Build a compact list of transparent submeshes, sort it with a bitonic or radix sort, and emit one indirect draw per submesh in sorted order. This avoids the readback and fits the existing GPU-driven path.
- Bucket sort. Quantize depth into, for example, 256 buckets and append submeshes to each bucket. This is cheaper than a full sort and bounds the error. The PlayStation ordering table used the same idea.

The third option is the best fit for the current collect pass. The draw key can include a bucket index, so submeshes in the same bucket stay batched. The buckets are drawn in order. The error is the depth range divided by 256, which is small for a typical scene.

The cost is one extra key per transparent submesh and one extra pass over the list. The benefit is that the common case, a few transparent objects at different depths, is correct.

Per-object sorting still fails for intersecting or large objects. The engine should document that limit and give artists a per-material priority override, as Unreal and Source do.

### 10.4 Weighted blended OIT

WBOIT fits the engine's forward renderer and its render graph. The pass structure:

1. `TransparentAccum` creates two transient images: `RGBA16F` for the accumulation and `R16F` for the revealage. It reads the opaque depth buffer and writes both transients. It draws the transparent geometry with additive blending. The fragment shader outputs `(C * a * w, a * w)` to the accumulation and `(0, a)` to the revealage, with the revealage blend set to `ZERO, ONE_MINUS_SRC_ALPHA`.
2. `TransparentResolve` reads the two transients, the scene color, and the depth. It divides the accumulation by its alpha, computes `1 - revealage`, and composites the result over the scene. It writes a new scene color transient.
3. The output pass reads the resolved scene color instead of the original.

The render graph needs a new scene color target for the resolve, because a pass cannot read and write the same image. That is one extra full-screen target at FP16, or a copy of the scene color. The cost is 8 bytes per pixel for the extra target and 10 bytes per pixel for the WBOIT buffers: about 37 MB at 1080p.

The fragment shader needs the view-space depth. The engine already passes the camera position and the view-projection matrix. The weight function should use the same depth scale as the scene. The constants from the paper assume a depth range of 0.1 to 500 units. A different scale needs a different normalization, or the weights will be wrong.

WBOIT does not need a depth sort. It does need the opaque depth buffer, so the opaque pass must run first. It also needs the transparent geometry to be identifiable, which the blend bits in the draw key already provide.

### 10.5 Alpha-to-coverage and hashed alpha

The engine does not currently use MSAA or temporal anti-aliasing. Alpha-to-coverage needs MSAA. Adding 4x MSAA to the scene target would multiply the color and depth storage by 4 and would need a resolve pass before the output. The change is large for foliage alone.

Hashed alpha needs no MSAA. It needs a good hash, a blue-noise pattern, and, ideally, a temporal filter. Without a temporal filter, the noise is visible. The engine could use a fixed blue-noise dither for foliage and accept the pattern, or add a simple temporal accumulation for transparent surfaces only.

A cheaper alternative is a distance fade. Foliage that is far away can switch to a coarser mesh or an opaque billboard. That reduces the number of alpha-tested fragments without changing the blend path.

### 10.6 What not to do

- Do not add a per-triangle sort before fixing the prepass. The prepass will still collapse the layers, and the sort will not show a visible improvement.
- Do not add a linked-list A-buffer without a memory budget. The worst case is unbounded, and a single particle system can exhaust the pool.
- Do not use WBOIT for colored glass. The method averages colors, and a red filter over a blue background will look purple.
- Do not use depth peeling as the default path. It is useful for validation, but its bandwidth cost is high and its pass count is data-dependent.

## 11. Open problems

Several parts of the problem are still unsolved.

**Colored transparency at low cost.** WBOIT and MBOIT approximate a scalar transmittance. A per-channel transmittance needs either a full spectral model or a small number of wavelength bands. Substrate's dual-source colored transmittance is a step, but it is not portable to every pipeline. The cost grows with the number of bands.

**Memory-bounded exact OIT.** Linked lists are exact until the pool runs out. A method that is exact and has a hard memory bound would remove the worst-case risk. Adaptive arrays are a step in that direction, but their layer cap is fixed.

**Temporal stability at low sample counts.** Stochastic methods need many samples. A denoiser that understands transparency could reconstruct a clean image from a few samples. Current denoisers are trained for opaque surfaces and for shadows, not for layered transparency. The EGSR 2025 work on alpha as coverage is one attempt to amortize the samples over time.

**Hardware support for ordered blending.** ROVs and fragment shader interlock make per-pixel lists safe, but they are not universal and they serialize fragments. A hardware blend unit that could accumulate layers in depth order would remove the need for most of this report. No such unit exists in consumer GPUs.

**Evaluation.** There is no standard benchmark for transparency quality. Papers report different scenes, different resolutions, and different error metrics. A shared benchmark with reference images and a cost model would make comparisons meaningful. FLIP is a better metric than PSNR, but it is not a benchmark.

## References

### History and compositing

- Andersson, P., Nilsson, J., Akenine-Möller, T., Oskarsson, M., Åström, K., and Fairchild, M. "FLIP: A Difference Evaluator for Alternating Images." Proceedings of the ACM on Computer Graphics and Interactive Techniques, 2020. https://research.nvidia.com/publication/flip
- Bagnell, D. "Weighted Blended Order-Independent Transparency." Cesium blog, 2014. https://cesium.com/blog/2014/03/14/weighted-blended-order-independent-transparency/
- Carpenter, L. "The A-buffer, an Antialiased Hidden Surface Method." SIGGRAPH 1984. https://doi.org/10.1145/800031.808585
- Copetti, R. "PlayStation Architecture." https://www.copetti.org/writings/consoles/playstation/
- id Software. Doom source, `r_things.c`. https://github.com/id-Software/DOOM
- id Software. Quake source, `gl_rsurf.c` and `gl_rmain.c`. https://github.com/id-Software/Quake
- id Software. Quake III Arena source, shader sort handling. https://github.com/id-Software/Quake-III-Arena
- id Software. Doom 3 source. https://github.com/id-Software/DOOM-3
- KallistiOS. `pvr_init_params_t` documentation, `autosort_disabled`. https://kos-docs.dreamcast.wiki/structpvr__init__params__t.html
- Nocash. "PSXSPX: PlayStation Specifications." https://problemkaputt.de/psx-spx.htm
- Nintendo. "Nintendo 64 Programming Manual." https://ultra64.ca/files/documentation/nintendo/Nintendo_64_Programming_Manual_NU6-06-0030-001G_HQ.pdf
- N64brew. "Reality Display Processor Pipeline." https://n64brew.dev/wiki/Reality_Display_Processor/Pipeline
- Porter, T. and Duff, T. "Compositing Digital Images." SIGGRAPH 1984. https://doi.org/10.1145/800031.808606
- Sega. "VDP1 User's Manual," command table and color calculation. http://docs.exodusemulator.com/Archives/SSDDV25/segahtml/hard/vdp1/hon/p05_40.htm
- Sega. "VDP1 User's Manual," features and VDP2 priority. http://docs.exodusemulator.com/Archives/SSDDV25/segahtml/hard/vdp1/hon/p01_13.htm
- Valve. Source SDK 2013. https://github.com/ValveSoftware/source-sdk-2013
- Vanilla Duke 3D source, `ENGINE.C`, `drawmasks`. https://github.com/fabiensanglard/vanilla_duke3D

### Order-independent transparency

- Bavoil, L. and Myers, K. "Order Independent Transparency with Dual Depth Peeling." NVIDIA, 2008. https://developer.download.nvidia.cn/SDK/10/opengl/src/dual_depth_peeling/doc/DualDepthPeeling.pdf
- Bavoil, L. and Enderton, E. "Constant-Memory Order-Independent Transparency Techniques." NVIDIA, 2011. https://developer.download.nvidia.com/assets/gamedev/files/sdk/11/ConstantMemoryOIT.pdf
- Enderton, E., Sintorn, E., Shirley, P., and Luebke, D. "Stochastic Transparency." I3D 2010. http://enderton.org/eric/pub/stochtransp-tvcg.pdf
- Everitt, C. "Interactive Order-Independent Transparency." NVIDIA, 2001. https://developer.download.nvidia.com/assets/gamedev/docs/order_independent_transparency.pdf
- Jansen, J. and Bavoil, L. "Fourier Opacity Mapping." I3D 2010. https://doi.org/10.1145/1730804.1730831
- Kerbl, B., Kopanas, G., Leimkühler, T., and Drettakis, G. "3D Gaussian Splatting for Real-Time Radiance Field Rendering." ACM Transactions on Graphics 42(4), 2023. https://arxiv.org/abs/2308.04079
- Liu, F., Huang, M.-C., Liu, X.-H., and Wu, E.-H. "Efficient Depth Peeling via Bucket Sort." HPG 2009. https://doi.org/10.1145/1572769.1572779
- Maule, M., Comba, J. L. D., Torchelsen, R., and Bastos, R. "Hybrid Transparency." I3D 2013. https://doi.org/10.1145/2448196.2448212
- McGuire, M. and Bavoil, L. "Weighted Blended Order-Independent Transparency." Journal of Computer Graphics Techniques 2(2), 2013. https://jcgt.org/published/0002/02/09/
- McGuire, M. and Mara, M. "Phenomenological Transparency." IEEE Transactions on Visualization and Computer Graphics, 2017. https://research.nvidia.com/publication/2017-03_phenomenological-transparency
- Münstermann, C., Krumpen, S., Klein, R., and Peters, C. "Moment-Based Order-Independent Transparency." Proceedings of the ACM on Computer Graphics and Interactive Techniques 1(1), 2018. https://doi.org/10.1145/3203206
- Myers, K. and Bavoil, L. "Stencil Routed K-Buffer." NVIDIA, 2007. https://developer.download.nvidia.com/SDK/10/direct3d/Source/StencilRoutedKBuffer/doc/StencilRoutedKBuffer.pdf
- Radl, L., Steiner, M., Parger, M., et al. "StopThePop: Sorted Gaussian Splatting for View-Consistent Real-time Rendering." 2024. https://arxiv.org/abs/2402.00525
- Salvi, M., Montgomery, J., and Lefohn, A. "Adaptive Transparency." HPG 2011. https://software.intel.com/sites/default/files/m/1/2/a/5/7/37944-adaptive_transparency_hpg11.pdf
- Salvi, M. and Vaidyanathan, K. "Multi-layer Alpha Blending." I3D 2014. https://doi.org/10.1145/2556700.2556705
- Tsopouridis, G., Fudos, I., and Vasilakis, A. "Neural Moment Transparency." Eurographics 2024 short paper. https://diglib.eg.org/items/337d5595-3405-4fcf-89c7-ccbde4d9c0bd
- Wyman, C. and McGuire, M. "Improved Alpha Testing Using Hashed Sampling." 2019. https://research.nvidia.com/labs/rtr/publication/wyman2019improved/
- Yang, J. C., Hensley, J., Grün, H., and Thibieroz, N. "Real-Time Concurrent Linked List Construction on the GPU." Computer Graphics Forum 29(4), 2010. https://doi.org/10.1111/j.1467-8659.2010.01725.x

### Research

- "EVER: Exact Volumetric Ellipsoid Rendering for Real-Time View Synthesis." 2024. https://arxiv.org/abs/2410.01804
- "Moment-Based 3D Gaussian Splatting: Resolving Volumetric Occlusion with Order-Independent Transmittance." 2025. https://arxiv.org/abs/2512.11800
- "NeuralTO: Neural Reconstruction and View Synthesis of Translucent Objects." ACM Transactions on Graphics, 2024. https://dl.acm.org/doi/10.1145/3658186
- "Spatio-Temporal Dithering for Order-Independent Transparency on Ray Tracing Hardware." EGSR 2025. https://diglib.eg.org/items/a66e25db-7545-4612-bba8-3df6083209e8
- "2D Gaussian Splatting for Geometrically Accurate Radiance Fields." 2024. https://arxiv.org/abs/2403.17888

### Engines

- Bevy. `AlphaMode`. https://docs.rs/bevy/latest/bevy/prelude/enum.AlphaMode.html
- Bevy. `core_3d` render phases. https://github.com/bevyengine/bevy/blob/main/crates/bevy_core_pipeline/src/core_3d/mod.rs
- Blender. "EEVEE Material Settings." https://docs.blender.org/manual/en/latest/render/eevee/material_settings.html
- Epic Games. "Lit Translucency." https://dev.epicgames.com/documentation/en-us/unreal-engine/lit-translucency-in-unreal-engine
- Epic Games. "Material Blend Modes." https://dev.epicgames.com/documentation/en-us/unreal-engine/material-blend-modes-in-unreal-engine
- Epic Games. "Rendering Settings in the Unreal Engine Project Settings." https://dev.epicgames.com/documentation/en-us/unreal-engine/rendering-settings-in-the-unreal-engine-project-settings
- Epic Games. "Single Layer Water Shading Model." https://dev.epicgames.com/documentation/en-us/unreal-engine/single-layer-water-shading-model-in-unreal-engine
- Epic Games. "Substrate Materials Overview." https://dev.epicgames.com/documentation/en-us/unreal-engine/overview-of-substrate-materials-in-unreal-engine
- Godot. "3D Rendering Limitations." https://docs.godotengine.org/en/stable/tutorials/3d/3d_rendering_limitations.html
- Godot. `BaseMaterial3D`. https://docs.godotengine.org/en/stable/classes/class_basematerial3d.html
- Google. "Filament Materials." https://google.github.io/filament/Materials.md.html
- Guerrilla Games. "Decima Visibility," GCAP 2017. https://www.guerrilla-games.com/media/News/Files/GCAP2017_DecimaVisibility.pdf
- Hillaire, S. "Physically Based and Unified Volumetric Rendering in Frostbite." 2015. https://www.slideshare.net/slideshow/physically-based-and-unified-volumetric-rendering-in-frostbite/51840934
- id Software. "id Tech 6" (DOOM), SIGGRAPH 2016 Advances in Real-Time Rendering. https://advances.realtimerendering.com/s2016/Siggraph2016_idTech6.pptx
- three.js. `WebGLRenderer`. https://threejs.org/docs/#api/en/renderers/WebGLRenderer
- three.js. `webgpu_oit` example. https://threejs.org/examples/webgpu_oit.html
- Unity. "HDRP Renderer and Material Priority." https://raw.githubusercontent.com/Unity-Technologies/Graphics/master/Packages/com.unity.render-pipelines.high-definition/Documentation~/Renderer-And-Material-Priority.md
- Unity. "HDRP Transparent Depth Prepass." https://raw.githubusercontent.com/Unity-Technologies/Graphics/master/Packages/com.unity.render-pipelines.high-definition/Documentation~/snippets/shader-properties/surface-options/transparent-depth-prepass.md
- Unity. "URP Lit Shader." https://docs.unity3d.com/Packages/com.unity.render-pipelines.universal@14.0/manual/lit-shader.html

### Hardware and APIs

- Arm. "Framebuffer Fetch in Vulkan." https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/framebuffer-fetch-in-vulkan
- Arm. "Pixel Local Storage on Arm Mali GPUs." https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/pixel-local-storage-on-arm-mali-gpus
- Imagination. "Do Not Use Alpha Blend Unnecessarily." https://docs.imgtec.com/starter-guides/powervr-architecture/html/topics/rules/do-not-use-alpha-blend-unnecessarily.html
- Imagination. "Use On-Chip Memory Efficiently for Deferred Rendering." https://docs.imgtec.com/starter-guides/powervr-architecture/html/topics/rules/do-use-on-chip-memory-efficiently-for-deferred-rendering.html
- Intel. "AOIT-Update." https://github.com/GameTechDev/AOIT-Update
- Khronos. "VK_EXT_fragment_shader_interlock." https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_fragment_shader_interlock.html
- Khronos. "VK_EXT_rasterization_order_attachment_access." https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_rasterization_order_attachment_access.html
- Khronos. "VK_QCOM_tile_shading." https://docs.vulkan.org/refpages/latest/refpages/source/VK_QCOM_tile_shading.html
- Microsoft. "Direct3D 12 Hardware Feature Levels." https://learn.microsoft.com/en-us/windows/win32/direct3d12/hardware-feature-levels
- Microsoft. "HLSL InterlockedMax." https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/interlockedmax
- Microsoft. "Opacity Micromaps." https://devblogs.microsoft.com/directx/d3d12-opacity-micromaps/
- Microsoft. "Rasterizer-Ordered Views." https://learn.microsoft.com/en-us/windows/win32/direct3d12/rasterizer-order-views
- Microsoft. "Variable-Rate Shading." https://microsoft.github.io/DirectX-Specs/d3d/VariableRateShading.html
- Mesa. "POPS: Primitive Ordered Pixel Shading." https://austriancoder.pages.freedesktop.org/mesa/drivers/amd/hw/pops.html
- Vulkan Hardware Database. `VK_EXT_fragment_shader_interlock` coverage. https://vulkan.gpuinfo.org/listfeaturesextensions.php?extension=VK_EXT_fragment_shader_interlock&platform=all

### Color and perception

- Beer-Lambert law. https://en.wikipedia.org/wiki/Beer%E2%80%93Lambert_law
- Forsyth, T. "Premultiplied Alpha" and "The sRGB Learning Curve." http://tomforsyth1000.github.io/blog.wiki.html
- Gritz, L. and d'Eon, E. "The Importance of Being Linear." GPU Gems 3, chapter 24. https://developer.nvidia.com/gpugems/gpugems3/part-iv-image-effects/chapter-24-importance-being-linear
- Khronos. "EXT_framebuffer_sRGB." https://registry.khronos.org/OpenGL/extensions/EXT/EXT_framebuffer_sRGB.txt
- Novak, J. "What Every Coder Should Know About Gamma." 2016. https://blog.johnnovak.net/2016/09/21/what-every-coder-should-know-about-gamma/
- Unity. "Applying Edge Padding to Alpha Textures." https://docs.unity3d.com/450/Documentation/Manual/HOWTO-alphamaps.html
- Wolfe, A., et al. "Scalar Spatiotemporal Blue Noise Masks." 2021. https://research.nvidia.com/publication/2021-12_scalar-spatiotemporal-blue-noise-masks
