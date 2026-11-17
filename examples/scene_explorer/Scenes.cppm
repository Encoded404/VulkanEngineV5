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

// Filters the catalog by a case-insensitive substring query and exact license
// and category values. An empty filter means "all", and the result holds
// indexes into `catalog` in catalog order. Exposed so the device-free tests can
// cover the asset library's search without a UI.
[[nodiscard]] std::vector<std::size_t> FilterCatalog(
    std::span<const Examples::Explorer::DownloadableAsset> catalog, std::string_view query,
    std::string_view license, std::string_view category);

} // namespace Examples::SceneExplorer
