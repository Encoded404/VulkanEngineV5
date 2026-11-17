#include <gtest/gtest.h>

import std;

import Examples.Explorer.Download;
import Examples.Explorer.Scene;
import Examples.SceneExplorer.Scenes;

namespace {

using Examples::Explorer::CatalogContext;
using Examples::Explorer::DownloadableAsset;
using Examples::Explorer::IsPermissiveLicense;
using Examples::Explorer::IsValidDownloadableAsset;
using Examples::Explorer::SceneCatalog;
using Examples::SceneExplorer::BuildCatalog;
using Examples::SceneExplorer::BuildDownloadCatalog;
using Examples::SceneExplorer::FilterCatalog;

[[nodiscard]] bool Contains(const std::vector<std::size_t>& indexes, const std::size_t expected) {
    return std::ranges::find(indexes, expected) != indexes.end();
}

} // namespace

// The cases below cover the data the generated header compiles in and the pure
// functions around it. Nothing here creates a Vulkan device, so they run in the
// default test preset.

TEST(SceneExplorerCatalogTest, CatalogEntriesPassTheDownloaderGate) {
    const std::vector<DownloadableAsset> catalog = BuildDownloadCatalog();
    ASSERT_FALSE(catalog.empty());
    for (const DownloadableAsset& asset : catalog) {
        std::string error{};
        EXPECT_TRUE(IsValidDownloadableAsset(asset, &error)) << asset.id << ": " << error;
    }
}

TEST(SceneExplorerCatalogTest, CatalogIdsAndFileNamesAreUnique) {
    const std::vector<DownloadableAsset> catalog = BuildDownloadCatalog();
    std::vector<std::string> ids{};
    std::vector<std::string> file_names{};
    for (const DownloadableAsset& asset : catalog) {
        EXPECT_EQ(std::ranges::find(ids, asset.id), ids.end()) << "duplicate id " << asset.id;
        EXPECT_EQ(std::ranges::find(file_names, asset.file_name), file_names.end())
            << "duplicate file name " << asset.file_name;
        ids.push_back(asset.id);
        file_names.push_back(asset.file_name);
    }
}

TEST(SceneExplorerCatalogTest, CatalogUsesOnlyPermissiveLicenses) {
    for (const DownloadableAsset& asset : BuildDownloadCatalog()) {
        EXPECT_TRUE(IsPermissiveLicense(asset.license)) << asset.id << ": " << asset.license;
    }
}

TEST(SceneExplorerCatalogTest, CatalogUrlsArePinned) {
    // A branch URL would let the bytes change under a fixed checksum.
    for (const DownloadableAsset& asset : BuildDownloadCatalog()) {
        EXPECT_EQ(asset.url.find("/main/"), std::string::npos) << asset.id;
        EXPECT_EQ(asset.url.find("/master/"), std::string::npos) << asset.id;
        EXPECT_EQ(asset.url.rfind("https://", 0), 0u) << asset.id;
    }
}

TEST(SceneExplorerCatalogTest, CatalogAttributionCoversCcBy) {
    for (const DownloadableAsset& asset : BuildDownloadCatalog()) {
        if (asset.license == "CC0-1.0") continue;
        EXPECT_FALSE(asset.attribution.empty()) << asset.id;
        EXPECT_FALSE(asset.source_page.empty()) << asset.id;
    }
}

TEST(SceneExplorerCatalogTest, CatalogHasCategories) {
    for (const DownloadableAsset& asset : BuildDownloadCatalog()) {
        EXPECT_FALSE(asset.category.empty()) << asset.id;
    }
}

TEST(SceneExplorerSceneTest, SceneCatalogHasThreeScenes) {
    const SceneCatalog catalog = BuildCatalog(CatalogContext{});
    ASSERT_EQ(catalog.size(), 3u);
    EXPECT_EQ(catalog[0].id, "sample-assets");
    EXPECT_EQ(catalog[1].id, "asset-library");
    EXPECT_EQ(catalog[2].id, "material-lab");
    for (const auto& scene : catalog) {
        EXPECT_TRUE(static_cast<bool>(scene.load)) << scene.id;
        EXPECT_TRUE(static_cast<bool>(scene.unload)) << scene.id;
        EXPECT_TRUE(static_cast<bool>(scene.update)) << scene.id;
        EXPECT_TRUE(static_cast<bool>(scene.options)) << scene.id;
        EXPECT_FALSE(scene.name.empty()) << scene.id;
        EXPECT_FALSE(scene.license.empty()) << scene.id;
    }
    EXPECT_EQ(catalog[1].license, "per-asset");
}

TEST(SceneExplorerLibraryTest, FilterCatalogMatchesCaseInsensitively) {
    const std::vector<DownloadableAsset> catalog = BuildDownloadCatalog();
    ASSERT_GE(catalog.size(), 3u);

    const std::vector<std::size_t> by_name = FilterCatalog(catalog, "TOY CAR", "", "");
    ASSERT_EQ(by_name.size(), 1u);
    EXPECT_EQ(catalog[by_name.front()].id, "toy-car");

    EXPECT_TRUE(Contains(FilterCatalog(catalog, "clearcoat", "", ""), 0u));
    EXPECT_TRUE(Contains(FilterCatalog(catalog, "wayfair", "", ""), 1u));
    EXPECT_TRUE(Contains(FilterCatalog(catalog, "wayfair", "", ""), 2u));
    EXPECT_TRUE(FilterCatalog(catalog, "no such asset", "", "").empty());
}

TEST(SceneExplorerLibraryTest, FilterCatalogHonorsLicenseAndCategory) {
    const std::vector<DownloadableAsset> catalog = BuildDownloadCatalog();
    EXPECT_EQ(FilterCatalog(catalog, "", "", "").size(), catalog.size());

    const std::vector<std::size_t> cc0 = FilterCatalog(catalog, "", "CC0-1.0", "");
    ASSERT_EQ(cc0.size(), 2u);
    for (const std::size_t index : cc0) EXPECT_EQ(catalog[index].license, "CC0-1.0");

    const std::vector<std::size_t> scenes = FilterCatalog(catalog, "", "", "Scenes");
    ASSERT_EQ(scenes.size(), 2u);
    for (const std::size_t index : scenes) EXPECT_EQ(catalog[index].category, "Scenes");

    const std::vector<std::size_t> both = FilterCatalog(catalog, "", "CC-BY-4.0", "Scenes");
    ASSERT_EQ(both.size(), 2u);
    for (const std::size_t index : both) {
        EXPECT_EQ(catalog[index].license, "CC-BY-4.0");
        EXPECT_EQ(catalog[index].category, "Scenes");
    }
}

TEST(SceneExplorerLibraryTest, FilterCatalogOnEmptyCatalogReturnsEmpty) {
    const std::vector<DownloadableAsset> empty{};
    EXPECT_TRUE(FilterCatalog(empty, "", "", "").empty());
    EXPECT_TRUE(FilterCatalog(std::span<const DownloadableAsset>{}, "toy", "", "").empty());
}
