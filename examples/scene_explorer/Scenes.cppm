module;

// The scene catalog for the explorer example.
export module Examples.SceneExplorer.Scenes;

import std;

import Examples.Explorer.Download;
import Examples.Explorer.Math;
import Examples.Explorer.Scene;

export namespace Examples::SceneExplorer {

// Builds the catalog the explorer will show. Asset discovery happens here, so
// the list is fixed for the lifetime of the process (use the scene's own
// rescan/reload controls to pick up new files at runtime).
[[nodiscard]] Examples::Explorer::SceneCatalog BuildCatalog(
    const Examples::Explorer::CatalogContext& context);

// The pinned on-demand download catalog (CC0 / CC BY 4.0 only). Exposed so
// `--list-downloads` and the panel share one list.
[[nodiscard]] std::vector<Examples::Explorer::DownloadableAsset> BuildDownloadCatalog();

} // namespace Examples::SceneExplorer
