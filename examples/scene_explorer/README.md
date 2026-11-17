# Scene Explorer

A catalog-driven viewer built on the shared `explorer_core` library. It ships
**no scene assets**: the glTF scene browses whatever directory you point it at,
the asset library downloads pinned samples on demand, and the Material Lab scene
is fully procedural.

```bash
# Procedural scene, no files needed
scene_explorer --scene material-lab

# Browser of everything on disk or already downloaded
scene_explorer --assets ~/assets/gltf-sample-assets

# Open one file directly
scene_explorer --asset ~/Downloads/ToyCar.glb

# Browse the pinned catalog, then download and open an entry in the panel
scene_explorer --scene asset-library

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
| `asset-library` | The pinned download catalog with search, license and category filters, install state, download progress, cancel, remove, and open-for-viewing. | no |
| `material-lab` | Procedural 5×5 sphere grid with metallic/roughness/emissive overrides, a controllable sun, spin, and three camera paths. | no |

`--download <id>` starts in `asset-library`, which selects that entry and starts
the transfer. `sample-assets` and `asset-library` share one downloader, so
exactly one transfer runs at a time and `--download` is honored once per
process. Opening a downloaded asset from the library gives it a camera orbit, so
Path mode is useful for every entry.

## On-demand downloads

Nothing is bundled. `examples/scene_explorer/catalog.json` lists a small catalog
of permissively licensed entries, with the license and attribution shown before
the download starts:

- Khronos glTF-Sample-Assets, all CC0 or CC BY 4.0: Toy Car, Sheen Chair,
  Iridescence Lamp, Commercial Refrigerator, Mosquito in Amber, A Beautiful Game.
- Two full environments from the Babylon.js Assets project, CC BY 4.0:
  Power Plant (43 MB, 65 embedded textures) and Haunted House (39 MB, 11
  embedded textures).

Files land in the per-user cache (`Storage::CacheDir()/scenes`, i.e. the
`--user-dir`/XDG data location, never the executable directory), are verified
against a pinned SHA-256, written with a `.license.txt` sidecar, and appear in
the asset combo afterwards. One transfer runs at a time; cancel and remove are in
the panel.

On a CPU/software Vulkan device the two environments take roughly 30-40 seconds
to decode and upload even though the download is fast (measured on lavapipe);
on a real GPU they open quickly. A Beautiful Game is the slowest of the Khronos
models for the same reason: many separate embedded images.

Switching assets releases the previous asset's materials, mesh and bindless
texture slots on unload, and a switch is deferred until the asynchronous texture
uploader has drained, so a heavy asset cannot exhaust memory for the next one.
The import cache lives on the shared session but is dropped on unload, which
means a reload always re-reads the file from disk.

`--list-downloads` prints `id | name | category | license | size | url` from the
same generated data the panel uses. The downloader itself is in
[`examples/explorer/Download.cppm`](../explorer/Download.cppm).

### Why the famous big scenes are not in the catalog

The catalog only takes assets that pass all three gates at once: a permissive
license, one downloadable file, and geometry the importer can read.

- **Sponza** is licensed under the Cryengine Limited License Agreement (the
  Khronos copy declares exactly that in its `LICENSE.md`), which is not
  permissive.
- **Amazon Lumberyard Bistro** is CC BY 4.0, but it is distributed as FBX/OBJ
  with separate textures, and the glTF conversions in circulation use
  `EXT_meshopt_compression` or Draco geometry, which this importer does not
  decode.
- **Sun Temple** and **Emerald Square** are CC BY-NC-SA (non-commercial,
  share-alike), **Virtual City** is under the 3DRT testing license, and
  **PlaysetLightTest** is CC BY-NC-SA.

Supporting those would mean multi-file downloads plus compressed-geometry
decode, not another catalog row.

## Credits and licenses

The build writes a `credits/` folder next to the executable:

```
credits/
  CREDITS.md              catalog statement and one row per asset
  LICENSE                 this project's MIT license
  assets/<id>.txt         name, license, attribution, source, URL, checksum
  licenses/<SPDX>.txt     verbatim license text for every license in the catalog
```

Nothing here is manual. `catalog.json` is the single source of truth:
configure time parses and validates it, emits
`build/generated/catalog/scene_explorer_catalog.hpp` (compiled into the binary, so
no JSON is read at runtime), and stages the credits folder; every build then
copies that folder to `bin/examples/<Config>/scene_explorer/credits`. A plain
`cmake --build` is enough, and a deleted `credits/` folder comes back on the next
build.

The configure step fails, rather than warning, when:

- the catalog `version` is not `1`;
- a field is missing, empty, or contains a newline or another control character;
- `license` is not `CC0-1.0`, `CC-BY-4.0`, `CC-BY-3.0` or `PUBLIC-DOMAIN`;
- no `licenses/<SPDX>.txt` exists for an entry's license;
- `url` is not HTTPS, or points at a branch (`/main/` or `/master/`);
- `file_name` or `id` contains a path separator or is `.`/`..`;
- `size` is not a positive integer, or `sha256` is not 64 lowercase hex digits;
- an `id` or `file_name` repeats.

A configure-time generator cannot be skipped by building a different target:
the executable depends on the credits deploy target, and the deploy fails if the
staging folder is missing. A hand-copied executable without `credits/` is the
only case the build cannot see, so startup logs a warning for it.

## Adding an entry

1. Add one object to `catalog.json` with all eleven fields. The URL must pin a
   commit SHA; a branch URL would let the bytes change under a fixed checksum.
2. If the license is new to the project, add `licenses/<SPDX>.txt` with its
   verbatim text. The license list is globbed at configure time, so reconfigure
   after adding a file (editing `catalog.json` already does that).
3. Build. The header, the panel, `--list-downloads` and `credits/` all pick the
   entry up together.

The two committed texts, `licenses/CC0-1.0.txt` and
`licenses/CC-BY-4.0.txt`, are the verbatim legal code published by
[Creative Commons](https://creativecommons.org/licenses/). Do not edit them; if
a text needs to change, replace it with the current official text.

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
- Assets are cached by path in the shared session: switching back and forth does
  not re-import or re-upload within one load. Downloaded files are picked up by
  the same scan (local `--assets` directory first, then the download cache).
- The downloader refuses to write past the catalog's declared size, so a
  changed upstream file can neither fill the disk nor be installed.
- Switching assets is a scene reload (`SceneContext::RequestReload`), applied at
  the next frame boundary by the explorer.
- The library's whole live UI is one `OptionsUi::Dynamic` callback, because the
  explorer declares a scene's options once per load. Its license and category
  filters are derived from the catalog every frame, so a new entry needs no
  extra UI state.
- The engine's mesh loader also understands `.obj` (geometry plus per-face
  material ids, no material import) and its internal `.bin` format, but the
  explorer is glTF-only: `--assets` only discovers `.glb`/`.gltf`, and the
  catalog's import path builds materials through the glTF importer. An `.obj`
  entry would need its own material path before it could join the catalog.
- `KHR_draco_mesh_compression` is unsupported: the mesh path logs the rejection
  and falls back to a placeholder quad, so a Draco asset appears as a quad
  rather than failing loudly.
- The engine's lit material contract covers core PBR plus unlit, emissive
  strength, texture transforms and basisu. `KHR_materials_transmission` /
  specular / clearcoat are ignored, so glass renders as opaque PBR.
