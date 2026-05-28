# Bounding Volume System

## Overview

Two-tier GPU occlusion culling:

1. **Sphere test** — cheap early rejection using a bounding sphere (center + radius).
2. **OBB test** — tighter fit for submeshes that pass the sphere test.
   Projects the 8 OBB corners to screen space, builds a 2D AABB, picks the
   Hi-Z level, and tests. Optionally refines at a finer mip level.

All bounding volumes are computed in local space at load time, stored per-
submesh, and transformed to world space by the model matrix in the shader.

---

## 1. CPU Data Structures

### MeshTypes.cppm — BoundingSphere / BoundingOBB

```cpp
struct BoundingSphere {
    Vector3 center;     // local-space center
    float   radius;     // local-space radius (max vertex distance from center)
};

struct BoundingOBB {
    Vector3 center;         // local-space center (independent from sphere)
    Vector3 axis_u;         float half_extent_u;
    Vector3 axis_v;         float half_extent_v;
    Vector3 axis_w;         float half_extent_w;
};
```

### SubMesh extension

```cpp
struct SubMesh {
    uint32_t index_start{0};
    uint32_t index_count{0};
    TechniqueId technique_id{0};
    TextureSlot texture_slot{0};
    BoundingSphere sphere{};
    BoundingOBB   obb{};
};
```

### GPU buffer structs (separate sphere + OBB)

Two separate block buffers so the sphere test only loads 16B per submesh.
The OBB buffer (64B per entry) is only fetched by threads that pass the
sphere test.

```glsl
// 16 bytes — sphere only. Matches BoundingSphere layout.
struct SphereGPU {
    vec4 center_radius;   // center.xyz + radius
};

// 64 bytes — OBB only. Matches BoundingOBB layout.
struct OBBGPU {
    vec4 center_pad;      // center.xyz + padding
    vec4 u_half;          // axis_u.xyz + half_extent_u
    vec4 v_half;          // axis_v.xyz + half_extent_v
    vec4 w_half;          // axis_w.xyz + half_extent_w
};
```

CPU and GPU layouts match exactly for memcpy.

---

## 2. Mesh Loader Architecture

### IMeshLoader base class (new)

A virtual base class that all mesh loaders derive from. The non-virtual
`Load()` method calls the format-specific `DoLoad()` then runs shared
post-processing:

```cpp
class IMeshLoader {
public:
    std::shared_ptr<Mesh> Load(const std::filesystem::path& path) {
        auto mesh = DoLoad(path);
        PostProcess(*mesh);
        return mesh;
    }

protected:
    virtual std::shared_ptr<Mesh> DoLoad(
        const std::filesystem::path& path) = 0;

    void PostProcess(Mesh& mesh) {
        ComputeBoundingVolumes(mesh);
    }
};
```

This ensures bounding volumes are always computed, regardless of which loader
is used. A dev writing a new loader just overrides `DoLoad()` and gets
bounding volumes for free.

### ComputeBoundingVolumes algorithm

Per-submesh, operating on the Mesh's `vertices` + `indices` arrays:

```
for each submesh sm in mesh.subMeshes:
    positions = []
    for i = sm.index_start .. sm.index_start + sm.index_count:
        idx = mesh.indices[i]
        positions.push(mesh.vertices[idx])

    // ── Bounding sphere (Ritter's algorithm) ──
    p1 = positions[0]
    for v in positions:
        if distance²(v, p1) > distance²(p2, p1): p2 = v
    for v in positions:
        if distance²(v, p2) > distance²(p1, p2): p1 = v
    center = (p1 + p2) * 0.5
    radius = distance(p1, center)
    for v in positions:
        d = distance(v, center)
        if d > radius:
            t = (d - radius) / (2 * d)
            center = center + (v - center) * t
            radius = (radius + d) * 0.5
    sm.sphere = { center, radius }

    // ── OBB centroid ──
    obb_center = average(positions)

    // ── OBB axes via PCA ──
    C = [0, 0, 0, 0, 0, 0]  // xx, xy, xz, yy, yz, zz
    for v in positions:
        d = v - obb_center
        C[0] += d.x*d.x    C[1] += d.x*d.y    C[2] += d.x*d.z
        C[3] += d.y*d.y    C[4] += d.y*d.z
        C[5] += d.z*d.z
    for i in 0..5: C[i] /= len(positions)

    // Eigen decomposition of 3×3 symmetric matrix
    // via Jacobi iteration (10 iterations for 3×3):
    //   A = [C0 C1 C2]
    //       [C1 C3 C4]
    //       [C2 C4 C5]
    //   eigenvectors = identity matrix
    //   repeat 10 times:
    //       find p,q with largest |A[p][q]|
    //       compute θ = 0.5 * atan2(2*A[p][q], A[p][p] - A[q][q])
    //       build rotation matrix J (θ at p,q)
    //       A = J^T * A * J
    //       eigenvectors = eigenvectors * J
    //   eigenvalues = diagonal of A
    //   sort eigenvectors by eigenvalue (descending)
    //   eigenvectors are the OBB axes

    (axis_u, axis_v, axis_w) = eigenvectors (sorted by eigenvalue)

    min_u = +inf, max_u = -inf
    min_v = +inf, max_v = -inf
    min_w = +inf, max_w = -inf
    for v in positions:
        d = v - obb_center
        proj_u = dot(d, axis_u)    min_u = min(min_u, proj_u)   max_u = max(max_u, proj_u)
        proj_v = dot(d, axis_v)    min_v = min(min_v, proj_v)   max_v = max(max_v, proj_v)
        proj_w = dot(d, axis_w)    min_w = min(min_w, proj_w)   max_w = max(max_w, proj_w)

    half_extent_u = max(abs(min_u), abs(max_u))
    half_extent_v = max(abs(min_v), abs(max_v))
    half_extent_w = max(abs(min_w), abs(max_w))

    sm.obb = { obb_center, axis_u, half_extent_u, axis_v, half_extent_v, axis_w, half_extent_w }
```

### Magic loader integration

`MeshMagicLoader` becomes a derived `IMeshLoader`. Its `DoLoad()` detects
the format (magic bytes / extension) and delegates to the appropriate
assembler. `PostProcess()` runs automatically via the base class.

Format-specific loaders (ObjMeshLoader, BinMeshLoader, GltfMeshLoader) are
thin wrappers around the existing assembler logic, inheriting `IMeshLoader`.

### Data flow

```
IMeshLoader::Load(path)
  └─ DoLoad(path) ── assembler-specific ── fills Mesh { vertices, indices, subMeshes }
  └─ PostProcess(mesh)
       └─ ComputeBoundingVolumes(mesh)
            └─ for each SubMesh: compute sphere + OBB from vertex positions
  └─ return shared_ptr<Mesh>

SceneLoader::LoadMeshFromFile(path)
  └─ IMeshLoader::Load(path) → shared_ptr<Mesh>
  └─ convert Mesh → LoadedMeshData (positions, normals, uvs, indices, submeshes with BV data)

UploadCombined(meshes)
  └─ adjust submesh index_start by global offset
  └─ return CombinedScene { submeshes (with BV data) }

PrepareCompute
  └─ for each submesh:
       └─ write bounding_spheres[ci] = sm.sphere             (memcpy 16B)
       └─ write bounding_obb[ci]     = sm.obb                (memcpy 64B)
```

---

## 3. GPU Data Pipeline

### DynEntry (expand.comp + CPU)

```glsl
// expand.comp — set 0, binding 0
struct DynEntry {
    vec3 pos;
    float pad0;          // alignment
    vec3 scale;          // full scale from entity transform
    float pad1;          // alignment to next vec4
    vec4 rot;
};
// Total: 48 bytes
```

CPU side (`SceneRendererFrame.cpp`):
```cpp
struct DynamicEntry {
    float px, py, pz, pad0;
    float sx, sy, sz, pad1;
    float rx, ry, rz, rw;
};
// Total: 48 bytes
```

Block buffer config: `entry_size=48`.

### modelMat()

```glsl
// modelMat = translate(p) * rotate(r) * scale(s)
mat4 modelMat(vec3 p, vec4 r, vec3 s) {
    float x = r.x, y = r.y, z = r.z, w = r.w;
    float xx = x*x, yy = y*y, zz = z*z;
    float xy = x*y, xz = x*z, xw = x*w;
    float yz = y*z, yw = y*w, zw = z*w;

    return mat4(
        (1.0 - 2.0*(yy + zz)) * s.x,    // column 0 = rot col 0 * scale.x
        2.0*(xy + zw) * s.x,
        2.0*(xz - yw) * s.x,
        0.0,

        2.0*(xy - zw) * s.y,            // column 1 = rot col 1 * scale.y
        (1.0 - 2.0*(xx + zz)) * s.y,
        2.0*(yz + xw) * s.y,
        0.0,

        2.0*(xz + yw) * s.z,            // column 2 = rot col 2 * scale.z
        2.0*(yz - xw) * s.z,
        (1.0 - 2.0*(xx + yy)) * s.z,
        0.0,

        p.x, p.y, p.z, 1.0              // column 3 = translation
    );
}
```

### Expand shader body

`maxScale` is computed in the expand shader and written to VertEntry:

```glsl
vec3 s = dyn[block].data[elemIdx].scale;
float maxScale = max(abs(s.x), max(abs(s.y), abs(s.z)));
mat4 mvp = pc.vp * modelMat(p, r, s);
vtx[block].data[elemIdx].MVP = mvp;
vtx[block].data[elemIdx].maxScale = maxScale;
```

### VertEntry

```glsl
// expand.comp — set 0, binding 2 (also occlusion shader set 0, binding 0)
struct VertEntry {
    mat4 MVP;
    float maxScale;
    uint textureSlot;
    // 8 bytes implicit padding (struct rounds to 80B = 5 × 16B)
};
```

Block buffer stays at 80 bytes per entry.

### Bounding volume block buffers

Two block buffers in `FrameResources`:

| Buffer | Per entry | Memory | Written by | Read by |
|---|---|---|---|---|
| `bounding_spheres` | 16B | HOST_VISIBLE \| HOST_COHERENT | CPU (PrepareCompute) | occlusion cull (sphere test) |
| `bounding_obb` | 64B | HOST_VISIBLE \| HOST_COHERENT | CPU (PrepareCompute) | occlusion cull (OBB test) |

Initialization in `SceneRenderer.cpp`:
```cpp
fr.bounding_spheres.Initialize(be,
    make_block_config(16, BLOCK_ENTRIES, {},
        vk::MemoryPropertyFlagBits::eHostVisible |
        vk::MemoryPropertyFlagBits::eHostCoherent));
fr.bounding_obb.Initialize(be,
    make_block_config(64, BLOCK_ENTRIES, {},
        vk::MemoryPropertyFlagBits::eHostVisible |
        vk::MemoryPropertyFlagBits::eHostCoherent));
```

### Occlusion set descriptor layout

| Binding | Contents | Type |
|---|---|---|
| 0 | submesh_vertex_data blocks (VertEntry) | storage buffer[] |
| 1 | submesh_cull buffer (CullEntry) | storage buffer[] |
| 2 | bounding_spheres blocks (SphereGPU) | storage buffer[] |
| 3 | Hi-Z depth texture | combined image sampler |
| 4 | bounding_obb blocks (OBBGPU) | storage buffer[] |

Descriptor pool sizing:
```cpp
// SceneRenderer.cpp — occlusion pool:
pc.max_storage_buffers = FRAMES_IN_FLIGHT * MAX_BLOCKS * 4;
```

---

## 4. Occlusion Shader (occlusion_sort.comp)

### Push constants

```glsl
layout(push_constant) uniform PC {
    uint elementCount;
    uint refineLevel;      // 0 = no refinement, 1 = one level finer, etc.
    uint hizWidth;         // Hi-Z base width  (mip 0)
    uint hizHeight;        // Hi-Z base height (mip 0)
} pc;
```

CPU side:
```cpp
struct OccPC {
    uint32_t cnt;
    uint32_t refine_level;
    uint32_t hiz_width;
    uint32_t hiz_height;
};
```

### Sphere test (early rejection)

```
id = gl_GlobalInvocationID.x
if id >= pc.elementCount: return

block = id / 256
elem  = id % 256

// Skip already-culled submeshes
if cull[block].data[elem].indexCount == 0: return

// Read data
mvp        = submeshVertexInfo[block].data[elem].MVP
maxScale   = submeshVertexInfo[block].data[elem].maxScale
sphereData = sphereBuf[block].data[elem]           // SphereGPU (binding 2)
sphereCenter = sphereData.center_radius.xyz         // LOCAL space
localRadius  = sphereData.center_radius.w

// Transform sphere center to clip space
clipPos = mvp * vec4(sphereCenter, 1.0)
if clipPos.w <= 0:
    cull[block].data[elem].indexCount = 0
    return

// World-space radius
R = localRadius * maxScale
// Near-point in clip space (standard depth: 0=near, 1=far)
frontW = max(clipPos.w - R, 0.001)
nearZ  = (clipPos.z - R) / frontW

// Screen-space radius for mip selection
screenRadius = abs(R * mvp[1][1] / clipPos.w)
mip = int(floor(log2(max(screenRadius, 1.0))))
mip = min(mip, maxMip)   // clamp to available Hi-Z levels

// Sample Hi-Z at sphere center
screenPos = clipPos.xy / clipPos.w
screenPos = screenPos * 0.5 + 0.5
screenPos.y = 1.0 - screenPos.y

hizDepth = textureLod(hizTex, screenPos, float(mip)).r
if nearZ > hizDepth + 0.001:
    cull[block].data[elem].indexCount = 0
    return        // ── EARLY EXIT: occluded by sphere test ──
```

### OBB test

Only reached if sphere test passes. Reads from the OBB buffer (binding 4)
to compute the 8 OBB corners in clip space, builds a 2D screen-space AABB,
picks the Hi-Z level, and tests. Optionally refines at finer levels.

```
// ── Read OBB data (separate buffer, binding 4) ──
OBBGPU obb = obbBuf[block].data[elem];

// ── 8 corners in clip space ──
int cornerCount = 0;
float nearZ_OBB = +inf;
vec2 minXY = vec2(+inf), maxXY = vec2(-inf);

// Pre-extract for readability
vec3  c   = obb.center_pad.xyz;
vec3  u   = obb.u_half.xyz;
float hu  = obb.u_half.w;
vec3  v   = obb.v_half.xyz;
float hv  = obb.v_half.w;
vec3  w   = obb.w_half.xyz;
float hw  = obb.w_half.w;

for (int su = -1; su <= 1; su += 2) {
for (int sv = -1; sv <= 1; sv += 2) {
for (int sw = -1; sw <= 1; sw += 2) {
    vec3 localPos = c + u*hu*su + v*hv*sv + w*hw*sw;
    vec4 cp = mvp * vec4(localPos, 1.0);
    if (cp.w <= 0.0) continue;

    vec3 ndc = cp.xyz / cp.w;
    nearZ_OBB = min(nearZ_OBB, ndc.z);
    minXY = min(minXY, ndc.xy);
    maxXY = max(maxXY, ndc.xy);
    cornerCount++;
}}}

// All corners behind camera → occluded
if (cornerCount == 0):
    cull[block].data[elem].indexCount = 0
    return

// ── Screen-space AABB ──
vec2 screenMin = minXY * 0.5 + 0.5;
vec2 screenMax = maxXY * 0.5 + 0.5;
screenMin.y = 1.0 - screenMin.y;
screenMax.y = 1.0 - screenMax.y;
screenMin = clamp(screenMin, 0.0, 1.0);
screenMax = clamp(screenMax, 0.0, 1.0);

// ── Choose Hi-Z level from AABB size ──
vec2 aabbSize = screenMax - screenMin;
float maxDim = max(aabbSize.x * pc.hizWidth, aabbSize.y * pc.hizHeight);
int mip = int(floor(log2(max(maxDim, 1.0))));
mip = min(mip, maxMip);

// ── Test at chosen mip level ──
vec2 centerUV = (screenMin + screenMax) * 0.5;
float hizDepth = textureLod(hizTex, centerUV, float(mip)).r;
if (nearZ_OBB > hizDepth + 0.001):
    cull[block].data[elem].indexCount = 0
    return        // ── OCCLUDED at coarse level ──

// ── Optional refinement (go finer, test multiple texels) ──
uint refine = pc.refineLevel;
while (refine > 0 && mip > 0):
    mip--
    refine--

    ivec2 dims = textureSize(hizTex, mip);
    vec2 uvMin = screenMin;
    vec2 uvMax = screenMax;

    ivec2 tMin = ivec2(floor(uvMin * vec2(dims)));
    ivec2 tMax = ivec2(ceil (uvMax * vec2(dims)));
    tMin = max(tMin, ivec2(0));
    tMax = min(tMax, dims - 1);

    int maxTexels = 64;
    int tested = 0;

    for (y = tMin.y; y <= tMax.y && tested < maxTexels; y++):
    for (x = tMin.x; x <= tMax.x && tested < maxTexels; x++, tested++):
        hizDepth = texelFetch(hizTex, ivec2(x, y), mip).r;
        if (nearZ_OBB > hizDepth + 0.001):
            cull[block].data[elem].indexCount = 0
            return    // ── OCCLUDED at finer level ──

// ── VISIBLE — indexCount unchanged ──
```

The refinement loop is bounded to 64 texels max. If the AABB is larger,
only the first 64 are checked — the submesh remains visible (false
negative is acceptable; false positive is not).

---

## 5. SceneRenderer

### FrameResources

```cpp
struct FrameResources {
    BlockBuffer compact_dynamic;       // 48B (pos, scale, rot)
    BlockBuffer compact_static;        // 16B
    BlockBuffer bounding_spheres;      // 16B (SphereGPU)
    BlockBuffer bounding_obb;          // 64B (OBBGPU)
    BlockBuffer submesh_vertex_data;   // 80B (contains maxScale)
    BlockBuffer submesh_cull;          // 16B
    // ... rest unchanged
};
```

### PrepareCompute

In the per-submesh loop, after writing `compact_dynamic` and
`compact_static`:

```cpp
if (auto* d = static_cast<DynamicEntry*>(fr.compact_dynamic.Get(ci))) {
    d->px = pos.x; d->py = pos.y; d->pz = pos.z;
    d->sx = scale.x; d->sy = scale.y; d->sz = scale.z;
    d->rx = rot.x; d->ry = rot.y; d->rz = rot.z; d->rw = rot.w;
}
```

### Descriptor writes

```cpp
WriteBlocks(fr.occlusion_set.GetHandle(), 2, fr.bounding_spheres, ...);
WriteBlocks(fr.occlusion_set.GetHandle(), 4, fr.bounding_obb, ...);
```

### DispatchOcclusion push constants

```cpp
void SceneRenderer::DispatchOcclusion(vk::CommandBuffer cmd, uint32_t fi) {
    const uint32_t hiz_w = (depth_width_ + 1) / 2;
    const uint32_t hiz_h = (depth_height_ + 1) / 2;
    OccPC pc{ current_entity_count_, 1, hiz_w, hiz_h };
    cmd.pushConstants(..., sizeof(OccPC), &pc);
    cmd.dispatch((current_entity_count_ + 63) / 64, 1, 1);
}
```

---

## 6. Files

### New files

| File | Contents |
|---|---|
| `engine/FileLoaders/Mesh/MeshLoaderBase.cppm` | `IMeshLoader` abstract base class |
| `engine/FileLoaders/Mesh/BoundingVolumeUtils.cpp` | `ComputeBoundingVolumes()` + PCA implementation |
| `engine/FileLoaders/Mesh/BoundingVolumeUtils.h` | Header for BV computation (if needed by non-module code) |

### Modified files

| File | Final state |
|---|---|
| `engine/Mesh/MeshTypes.cppm` | Contains `BoundingSphere`, `BoundingOBB` structs; `SubMesh` has `sphere` and `obb` fields |
| `engine/FileLoaders/Mesh/MeshMagicLoader.cppm` | Inherits `IMeshLoader`, delegates `DoLoad()` to format-specific |
| `engine/FileLoaders/Mesh/BinMeshAssembler.cppm` | Wrapped in `BinMeshLoader : IMeshLoader` |
| `engine/FileLoaders/Mesh/ObjMeshAssembler.cppm` | Wrapped in `ObjMeshLoader : IMeshLoader` |
| `engine/FileLoaders/Mesh/GltfMeshAssembler.cppm` | Wrapped in `GltfMeshLoader : IMeshLoader` |
| `engine/SceneLoader/SceneLoader.cppm` | `LoadedMeshData` and `CombinedScene` carry BV data through submeshes |
| `engine/SceneLoader/SceneLoader.cpp` | `UploadCombined` adjusts submesh index_start + carries BV data |
| `engine/SceneRenderer/SceneRenderer.cppm` | `FrameResources` has `bounding_obb` |
| `engine/SceneRenderer/SceneRenderer.cpp` | Inits `bounding_obb` block buffer (64B), occlusion binding 4, pool 4x |
| `engine/SceneRenderer/SceneRendererFrame.cpp` | Writes `DynamicEntry` with `scale.xyz` (48B); writes `bounding_spheres` + `bounding_obb` per submesh; OccPC includes `refineLevel` + hiz dims |
| `engine/shaders/expand.comp` | `DynEntry` has `vec3 scale`; `modelMat()` takes `vec3 s`; computes `maxScale`; `VertEntry` has `maxScale` |
| `engine/shaders/occlusion_sort.comp` | Two-tier sphere + OBB test; reads `SphereGPU` (binding 2) + `OBBGPU` (binding 4) |

---

## 7. Edge Cases & Notes

### Scale in modelMat

`modelMat` accepts a full `vec3 scale` and applies per-axis scaling as
`translate(p) * rotate(r) * scale(s)`. Non-uniform scaling is handled
correctly: each column of the rotation matrix is multiplied by its
corresponding scale component.

`maxScale = max(abs(scale))` is computed in the expand shader and used only
for the sphere near-point test (conservative: overestimates the world-space
radius, never underestimates). The OBB corners are transformed by the full
MVP, so each axis is correctly scaled non-uniformly.

### Negative scale

`max_scale = max(abs(scale.x), abs(scale.y), abs(scale.z))` prevents
negative scale from breaking the test.

### Sphere center vs. OBB center

These are independent. The sphere uses Ritter's algorithm (may shift center
to minimize radius). The OBB center is the vertex centroid (PCA requires
centered data). The sphere buffer is always loaded (16B per submesh during
the sphere test). The OBB buffer is only loaded by threads that reach the
OBB test path, saving memory bandwidth on occluded submeshes.

### Refinement loop limit

The refinement loop is limited to 64 texel fetches per submesh to prevent
workgroup stalls from large AABBs. If an object with a huge screen-space
footprint passes the coarse test but has more than 64 texels at the finer
level, only the first 64 are checked. The object remains visible — a
potential false negative (overdraw) but never a false positive (missing
geometry).

### Thread divergence

The OBB test is per-submesh. Threads in the same workgroup may take
different paths (sphere-only vs. sphere+OBB vs. early-out). This is
acceptable: the sphere test is the common path (most objects are either
visible or occluded-trivially), and the OBB + refinement paths converge
quickly.

### PCA implementation

The Jacobi iteration for the 3×3 covariance matrix uses 10 iterations. A
fixed iteration count avoids dynamic loop exit and ensures deterministic
compile times.

### Hi-Z level clamping

The chosen mip level must be clamped to `[0, hiz_mip_count_ - 1]` to
avoid sampling outside the Hi-Z image.

### Pipeline barrier

No special barriers needed. Both `bounding_spheres` and `bounding_obb` are
`HOST_VISIBLE | HOST_COHERENT`, written before `DispatchOcclusion`, and
read by the occlusion shader.
