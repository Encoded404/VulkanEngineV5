# Scene Explorer

A catalog-driven viewer built on the shared `explorer_core` library. It ships
**no scene assets**: the glTF scene browses whatever directory you point it at,
and the Material Lab scene is fully procedural.

```bash
# Procedural scene, no files needed
scene_explorer --scene material-lab

# Browse a directory of .glb/.gltf files
scene_explorer --assets ~/assets/gltf-sample-assets

# Open one file directly
scene_explorer --asset ~/Downloads/ToyCar.glb

# Start in a specific mode / scene
scene_explorer --scene sample-assets --mode freefly
scene_explorer --list-scenes

# On-demand: fetch one catalog asset (pinned URL + SHA-256), then open it
scene_explorer --download toy-car
scene_explorer --list-downloads
```

## Scenes

| id | Contents | Needs files |
|---|---|---|
| `sample-assets` | Combo box over `.glb`/`.gltf` in `--assets` (or `--asset`), with live material overrides, spin, scale, rescan and reload. | yes |
| `material-lab` | Procedural 5×5 sphere grid with metallic/roughness/emissive overrides, a controllable sun, spin, and three camera paths. | no |

## On-demand downloads

Nothing is bundled. The **Download** section in the panel lists a small catalog
of Khronos glTF-Sample-Assets entries that are all CC0 or CC BY 4.0 (Toy Car, Sheen Chair,
Iridescence Lamp, Commercial Refrigerator, Mosquito in Amber, A Beautiful Game), with the license and
attribution shown before the download starts. Files land in the
per-user cache (`Storage::CacheDir()/scenes`, i.e. the `--user-dir`/XDG data
location, never the executable directory), are verified against a pinned
SHA-256, written with a `.license.txt` sidecar, and appear in the asset combo
afterwards. One transfer runs at a time; cancel and remove are in the panel.

On a CPU/software Vulkan device the largest entries take tens of seconds to
decode even though the download is fast; on a real GPU they open quickly.

Switching assets releases the previous asset's materials, mesh and bindless
texture slots on unload, and a switch is deferred until the asynchronous texture
uploader has drained, so a heavy asset cannot exhaust memory for the next one.

The catalog lives in `Scenes.cpp` (`BuildDownloadCatalog`): each entry pins a
commit-SHA URL, exact byte size, checksum, SPDX license and attribution. Adding
an asset means adding those five facts — `IsValidDownloadableAsset` refuses
anything that is not HTTPS, not a single path component, not CC0/CC BY, or not a
64-hex digest. The downloader itself is in
[`examples/explorer/Download.cppm`](../explorer/Download.cppm).

## Controls

| Input | Action |
|---|---|
| `W` `A` `S` `D`, `Q`/`E` | Freefly move / down / up (`Shift` boosts) |
| Right mouse drag | Freefly look |
| Left mouse drag | Showcase orbit |
| Middle mouse drag | Showcase pan |
| Mouse wheel | Zoom (Showcase) / move speed (Freefly) |
| `1` `2` `3` | Freefly / Showcase / Path |
| `F` | Frame the scene |
| `Esc` | Quit |

## Where to get assets (licensing)

The explorer displays the per-scene license in its list, but the asset choice is
yours. Only use content you can actually share:

- **CC0** (no obligations): [Poly Haven](https://polyhaven.com/license),
  [Kenney](https://kenney.nl/assets), [ambientCG](https://ambientcg.com).
- **CC BY 4.0** (commercial use OK, attribution required): Khronos
  [glTF-Sample-Assets](https://github.com/KhronosGroup/glTF-Sample-Assets)
  models tagged CC BY 4.0, Amazon Lumberyard Bistro (ORCA), Intel's Sponza Base
  Scene.
- **Avoid**: the Khronos *Sponza* model files (Cryengine EULA, not CC), Virtual
  City (testing-only 3DRT license), Sun Temple / Emerald Square (CC BY-NC-SA),
  Damaged Helmet (mixed NC component), and anything under a Sketchfab standard
  license. See `Models-issues.md` in the glTF-Sample-Assets repository for the
  models Khronos itself flags.

Keep the asset's `LICENSE`/attribution file next to the download; the scene list
shows the catalog's license string, not a per-file scan.

## Implementation notes

- glTF materials are imported through `ImportGltfMaterials` and registered as
  `DefaultMeshTechnique` materials; textures go through
  `GameEngine::LoadTextureFromGltfImage`, so KTX2/basisu, PNG and JPEG all work,
  including the buffer-view images that every textured `.glb` uses.
  Materials without a base-color texture get a generated 1×1 swatch instead of
  the checkerboard fallback.
- Assets are cached by path in the scene state: switching back and forth does
  not re-import or re-upload. Downloaded files are picked up by the same scan
  (local `--assets` directory first, then the download cache).
- The downloader refuses to write past the catalog's declared size, so a
  changed upstream file can neither fill the disk nor be installed.
- Switching assets is a scene reload (`SceneContext::RequestReload`), applied at
  the next frame boundary by the explorer.
- `KHR_draco_mesh_compression` is unsupported: the mesh path logs the rejection
  and falls back to a placeholder quad, so a Draco asset appears as a quad
  rather than failing loudly.
- The engine's lit material contract covers core PBR plus unlit, emissive
  strength, texture transforms and basisu. `KHR_materials_transmission` /
  specular / clearcoat are ignored, so glass renders as opaque PBR.
