
#include <CLI/CLI.hpp>
#include <logging/logging_macros.hpp>

import std;

import logiface;

import App.Game;
import App.Overrides;
import VulkanEngine.Application;
import VulkanEngine.Startup;
import Runtime.Application;

// CLI11 2.5.0's default Formatter runs the footer through
// detail::streamOutAsParagraph, which strips leading whitespace and collapses
// multi-space runs — destroying the aligned overrides table. Emit the footer
// verbatim; everything else matches the default Formatter.
class RawFooterFormatter final : public CLI::Formatter {
public:
    std::string make_help(const CLI::App* app, std::string name,
                          CLI::AppFormatMode mode) const override {
        if (mode == CLI::AppFormatMode::Sub) {
            return make_expanded(app, mode);
        }
        std::stringstream out;
        if (app->get_name().empty() && app->get_parent() != nullptr) {
            if (app->get_group() != "SUBCOMMANDS") {
                out << app->get_group() << ':';
            }
        }
        CLI::detail::streamOutAsParagraph(
            out, make_description(app), description_paragraph_width_, "");
        out << make_usage(app, name);
        out << make_positionals(app);
        out << make_groups(app, mode);
        out << make_subcommands(app, mode);
        out << make_footer(app);  // verbatim — no paragraph re-wrap
        return out.str();
    }
};

int main(int argc, char* const argv[]) {
    CLI::App app{"VulkanEngineV5 Demo"};
    app.formatter(std::make_shared<RawFooterFormatter>());

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

    bool no_validation = false;
    app.add_flag("--no-validation", no_validation, "Disable the Vulkan validation layer");

    App::Overrides::Overrides overrides{};
    std::vector<std::string> raw_overwrites;
    app.add_option("--overwrite", raw_overwrites,
                   "Testing overrides; run --help for the full key list")
       ->transform([&](std::string kv) {            // CLI11 2.5.0: the transform's
           try {                                    // return value REPLACES the stored
               App::Overrides::ApplyOverride(kv, overrides);   // value, so errors must
               return kv;                           // be thrown, not returned
           } catch (const App::Overrides::OverrideError& e) {
               throw CLI::ValidationError(std::string{e.what()});
           }
       });

    std::vector<std::string> force_disabled_extensions;
    app.add_option("--force-disable-ext", force_disabled_extensions,
                   "[deprecated] Force-disable a Vulkan extension for testing fallback paths; use --overwrite ext.disable=<name>");

    // Help fires during parse, so the footer must be set before; the pristine
    // struct makes "default" and "current" show the programmatic defaults.
    app.footer(App::Overrides::GenerateOverridesHelp(App::Overrides::Overrides{}));

    CLI11_PARSE(app, argc, argv);

    // Fold the deprecated flag into the registry sink, then log the effective
    // overrides BEFORE any move leaves the vector empty. The logger is
    // initialized here (idempotent; RunApplication re-applies the level).
    overrides.force_disabled_extensions.insert(overrides.force_disabled_extensions.end(),
                                               force_disabled_extensions.begin(),
                                               force_disabled_extensions.end());
    VulkanEngine::Startup::InitializeLogger(log_level_str);
    LOGIFACE_LOG(info, "effective overrides: " + App::Overrides::DescribeEffective(overrides));

    const std::filesystem::path executable_path = std::filesystem::absolute(std::filesystem::path(argv[0]));
    auto game = std::make_unique<App::Game::DemoGame>(render_mode, executable_path,
        model_path.empty() ? std::filesystem::path{} : std::filesystem::path{model_path},
        texture_path.empty() ? std::filesystem::path{} : std::filesystem::path{texture_path},
        overrides.gpl_policy, overrides.gpl_structure);

    VulkanEngine::Application::ApplicationConfig app_config{};
    app_config.app_name = "VulkanEngineV5 Demo";
    app_config.log_level = log_level_str;
    app_config.bootstrap_config.enable_validation = !no_validation;
    app_config.bootstrap_config.force_disabled_extensions = std::move(overrides.force_disabled_extensions);

    return VulkanEngine::Application::RunApplication(app_config, game->GetHooks());
}
