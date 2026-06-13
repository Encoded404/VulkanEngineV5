
#include <CLI/CLI.hpp>

import std;

import App.Game;
import VulkanEngine.Application;

int main(int argc, char* const argv[]) {
    CLI::App app{"VulkanEngineV5 Demo"};

    std::string log_level_str = "info";
    app.add_option("-l,--log-level", log_level_str, "Console output log level (trace, debug, info, warn, error, critical)")
       ->check(CLI::IsMember({"trace", "debug", "info", "warn", "error", "critical"}));

    App::Game::RenderMode render_mode = App::Game::RenderMode::Normal;
    app.add_option("-m,--mode", render_mode, "Rendering mode")
       ->transform(CLI::CheckedTransformer(std::map<std::string, App::Game::RenderMode>{
           {"normal", App::Game::RenderMode::Normal},
           {"normals", App::Game::RenderMode::Normals},
           {"no-textures", App::Game::RenderMode::NoTextures}
       }, CLI::ignore_case));

    std::string model_path;
    app.add_option("-M,--model", model_path, "Path to a .bin model file to load");

    std::string texture_path;
    app.add_option("-T,--texture", texture_path, "Path to a texture file to load (.png, .jpg, .ktx)");

    CLI11_PARSE(app, argc, argv);

    const std::filesystem::path executable_path = std::filesystem::absolute(std::filesystem::path(argv[0]));
    auto game = std::make_unique<App::Game::DemoGame>(render_mode, executable_path,
        model_path.empty() ? std::filesystem::path{} : std::filesystem::path{model_path},
        texture_path.empty() ? std::filesystem::path{} : std::filesystem::path{texture_path});

    VulkanEngine::Application::ApplicationConfig app_config{};
    app_config.app_name = "VulkanEngineV5 Demo";
    app_config.log_level = log_level_str;

    return VulkanEngine::Application::RunApplication(app_config, game->GetHooks());
}
