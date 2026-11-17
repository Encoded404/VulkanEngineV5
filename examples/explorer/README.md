# Shared scene explorer

Reusable half of the scene explorer: camera modes, the declarative options
panel, the scene catalog types, and the application that wires them to the
engine. `examples/scene_explorer` is one host; any example can link
`explorer_core` and hand `ExplorerApp` a different catalog.

## What a host gets

- **Three camera modes** (`Examples.Explorer.Camera`)
  - `Freefly` — WASD + Q/E, drag-to-look, wheel changes travel speed.
  - `Showcase` — orbit around the scene's focus point, drag to rotate, wheel to
    zoom, middle-drag to pan, optional turntable auto-rotate. This is the
    "model in the middle" mode.
  - `Path` — follows one of the scene's 0..N camera paths, with play/pause,
    loop, speed (world units per second) and a scrub slider.
  - Switching modes preserves the current position and view direction, so there
    is no jump cut. `F` frames the scene. Auto near/far planes scale with the
    scene radius.
- **Dynamic options** (`Examples.Explorer.Options`)
  - A scene declares controls with plain calls (`ui.SliderFloat("Roughness",
    state.roughness, 0, 1)`); the explorer draws them in its panel. No
    `<imgui.h>` in scene code, no manual widget ids, no reflection.
  - `Selectable(label, selected, on_click)` draws a clickable row (the asset
    library's list) and `InputText(label, std::string&, max_length)` a bound
    text field (its search box). Both keep `MakeLabel`'s stable id and the
    tooltip behavior of the other widgets.
  - `Dynamic(std::function<void(OptionsUi&)>)` rebuilds a sub-panel every frame
    for content that depends on live state. Use it whenever the set of visible
    controls changes (a download in progress, a filtered list); a value that
    only changes what `load` does belongs in the once-per-load callbacks.
  - `Custom(std::function<void()>)` is the escape hatch for anything the
    built-in controls do not cover.
- **On-demand downloads** (`Examples.Explorer.Download`)
  - The binary carries only a catalog of pinned HTTPS URLs plus a SHA-256 per
    asset; bytes are fetched when the user asks for them. `category` is
    presentation metadata for a browsing host; the downloader ignores it.
  - A permissive-license gate rejects anything that is not CC0 / CC BY before a
    transfer starts, and every entry is validated (HTTPS only, single-component
    file name, 64-hex checksum, non-zero size).
  - The transfer runs on a worker thread with progress and cancellation,
    enforces the declared byte count, verifies the digest, then installs
    atomically (`.part` → rename) and writes a `<id>.license.txt` sidecar with
    the attribution and source URL.
  - The host supplies the cache root (`Storage::CacheDir() / "scenes"` in the
    example); nothing is ever written next to the executable.

  ```cpp
  SceneDownloader downloader(storage.CacheDir() / "scenes");
  downloader.SetCatalog(entries);
  downloader.Start(entry);                    // false if one is already running
  // once per frame:
  const auto progress = downloader.Progress(); // fraction, state, error
  if (auto outcome = downloader.TakeOutcome()) { /* reload the scene */ }
  ```

- **Scene lifecycle** (`Examples.Explorer.Scene`)
  - A `SceneDefinition` is data: `load`, optional `unload`/`update`/`options`,
    and a `std::vector<CameraPath>`.
  - `SceneBuildResult::paths` lets one load replace those static paths for the
    scene it just built. The asset library uses it to offer an orbit around
    whatever model it opened; an empty vector keeps the definition's paths.
  - `SceneContext::CreateMeshEntity` creates the `Transform` + `MeshReference`
    pair; every entity a scene creates is returned in `SceneBuildResult` so the
    explorer can destroy it on a switch.
  - `SceneContext::RequestReload()` asks the explorer to rebuild the scene at
    the next frame boundary (used when an option changes what has to be
    re-imported, e.g. a different asset or grid size).

## Adding a scene

```cpp
Examples::Explorer::SceneDefinition MakeMyScene() {
    SceneDefinition scene{};
    scene.id = "my-scene";
    scene.name = "My Scene";
    scene.license = "CC0-1.0";          // shown in the scene list

    scene.load = [](SceneContext& ctx) {
        SceneBuildResult result{};
        const std::uint32_t mesh = ctx.engine->GetMeshRegistry().Register(/* ... */);
        result.entities.push_back(ctx.CreateMeshEntity(mesh));
        result.bounds = /* world-space Bounds */;
        return result;
    };

    scene.update = [](SceneContext& ctx, float dt) { /* animate/materials */ };
    scene.options = [](OptionsUi& ui) {
        ui.Section("My options");
        ui.Checkbox("Spin", state.spin);
    };
    scene.paths.push_back(/* CameraPath */);
    return scene;
}
```

Register it by appending to the `SceneCatalog` your host builds.

## Rules of thumb

- `load` runs at a frame boundary, not mid-render. It may create entities,
  register materials and upload textures.
- `update` runs once per rendered frame before `GameEngine::FrameUpdate`; keep
  GPU re-uploads (lighting, material data) behind a dirty flag.
- Bounds are world-space. The explorer frames them and derives clip planes from
  them, so return the real extents (the glTF importer keeps node placement on
  the submesh; use `ComputeBounds`).
- Paths are optional: a scene with none simply disables Path mode.
