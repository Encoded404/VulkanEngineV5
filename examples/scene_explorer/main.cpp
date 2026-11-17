#include <CLI/CLI.hpp>

#include "engine/core/bootstrap/EntryPoint.hpp"

import std;

import Runtime.Application;
import Runtime.ExampleCli;
import Runtime.Overrides;
import VulkanEngine.Application;
import Examples.Explorer.App;
import Examples.Explorer.Camera;
import Examples.Explorer.Scene;
import Examples.SceneExplorer.Scenes;

namespace {

[[nodiscard]] bool ParseCameraMode(const std::string& text, Examples::Explorer::CameraMode& out) {
    if (text == "freefly") {
        out = Examples::Explorer::CameraMode::Freefly;
        return true;
    }
    if (text == "showcase") {
        out = Examples::Explorer::CameraMode::Showcase;
        return true;
    }
    if (text == "path") {
        out = Examples::Explorer::CameraMode::Path;
        return true;
    }
    return false;
}

} // namespace

int VulkanEngine::AppMain(int argc, char* const argv[]) {
    Runtime::Cli cli{"VulkanEngineV5 Scene Explorer", VKENGINE_ORG_ID, VKENGINE_APP_ID};

    std::string assets_dir;
    cli.App().add_option("--assets", assets_dir,
                         "Directory scanned for .glb/.gltf assets (default: <exe>/assets)");

    std::vector<std::string> asset_files;
    cli.App().add_option("--asset", asset_files,
                         "Add one .glb/.gltf file to the catalog (repeatable)");

    std::string scene_id;
    cli.App().add_option("--scene", scene_id, "Scene id to open (see --list-scenes)");

    std::string mode_name = "showcase";
    cli.App().add_option("--mode", mode_name, "Camera mode: freefly, showcase, path");

    bool list_scenes = false;
    cli.App().add_flag("--list-scenes", list_scenes, "Print the scene catalog and exit");

    std::string download_id;
    cli.App().add_option("--download", download_id,
                         "Download a catalog asset by id, then open it (implies --scene asset-library)");

    bool list_downloads = false;
    cli.App().add_flag("--list-downloads", list_downloads,
                       "Print the on-demand download catalog (id, name, category, license, "
                       "size, url) and exit");

    if (!cli.Parse(argc, argv)) {
        return cli.ExitCode();
    }

    Examples::Explorer::CameraMode initial_mode = Examples::Explorer::CameraMode::Showcase;
    if (!ParseCameraMode(mode_name, initial_mode)) {
        std::cerr << "Unknown --mode '" << mode_name << "' (expected freefly, showcase or path)\n";
        return 1;
    }

    const std::filesystem::path exe_dir = cli.ExecutablePath().parent_path();

    Examples::Explorer::CatalogContext catalog_context{};
    catalog_context.exe_dir = exe_dir;
    catalog_context.asset_dir =
        assets_dir.empty() ? (exe_dir / "assets") : std::filesystem::path{assets_dir};
    for (const std::string& file : asset_files) {
        catalog_context.asset_files.emplace_back(file);
    }

    Examples::Explorer::SceneCatalog catalog = Examples::SceneExplorer::BuildCatalog(catalog_context);

    if (list_scenes) {
        for (const auto& scene : catalog) {
            std::cout << scene.id << "  |  " << scene.name << "  |  " << scene.license << '\n';
        }
        return 0;
    }

    if (list_downloads) {
        for (const auto& asset : Examples::SceneExplorer::BuildDownloadCatalog()) {
            std::cout << asset.id << "  |  " << asset.name << "  |  " << asset.category
                      << "  |  " << asset.license << "  |  " << asset.size << " bytes  |  "
                      << asset.url << '\n';
        }
        return 0;
    }

    Examples::Explorer::ExplorerConfig config{};
    config.exe_dir = exe_dir;
    config.asset_dir = catalog_context.asset_dir;
    config.initial_scene = scene_id;
    config.initial_action = download_id;
    config.initial_mode = initial_mode;
    if (!download_id.empty() && config.initial_scene.empty()) {
        // The library shows the catalog, the progress and the entry the download
        // is for; `sample-assets` stays the browse-your-own-files scene.
        config.initial_scene = "asset-library";
    }
    config.gpl_policy = cli.GetOverrides().gpl_policy;
    config.gpl_structure = cli.GetOverrides().gpl_structure;

    auto app = std::make_unique<Examples::Explorer::ExplorerApp>(std::move(config), std::move(catalog));

    return VulkanEngine::Application::RunApplication(cli.MakeConfig(), app->GetHooks());
}
