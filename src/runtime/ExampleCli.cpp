module;

#include <CLI/CLI.hpp>

#include <logging/logging_macros.hpp>

module Runtime.ExampleCli;

import std;

import logiface;

import VulkanEngine.Application;
import VulkanEngine.Startup;
import Runtime.Overrides;

namespace Runtime {

Cli::Cli(std::string_view app_name)
    : app_(std::string{app_name})
    , app_name_(app_name) {
    app_.formatter(std::make_shared<RawFooterFormatter>());

    app_.add_option("-l,--log-level", log_level_,
                    "Console output log level (trace, debug, info, warn, error, critical)")
       ->check(CLI::IsMember({"trace", "debug", "info", "warn", "error", "critical"}));

    app_.add_flag("--validation", force_validation_, "Enable the Vulkan validation layer");
    app_.add_flag("--no-validation", force_no_validation_, "Disable the Vulkan validation layer");

    app_.add_option("--overwrite", overwrite_values_,
                    "Testing overrides; run --help for the full key list")
       ->transform([this](std::string kv) {            // CLI11 2.5.0: the transform's
           try {                                       // return value REPLACES the stored
               overrides_.Apply(kv);                   // value, so errors must be
               return kv;                              // thrown, not returned
           } catch (const OverrideError& e) {
               throw CLI::ValidationError(std::string{e.what()});
           }
       });

    app_.add_option("--force-disable-ext", force_disabled_extensions_,
                    "[deprecated] Force-disable a Vulkan extension for testing fallback paths; use --overwrite ext.disable=<name>");
}

void Cli::RegisterOverride(OverrideSpec spec) {
    overrides_.Register(std::move(spec));
}

bool Cli::Parse(int argc, char* const argv[]) {
    executable_path_ = std::filesystem::absolute(std::filesystem::path(argv[0]));

    // Help fires during parse, so the footer must be set before; the pristine
    // registry makes "default" and "current" show the programmatic defaults.
    app_.footer(overrides_.GenerateHelp());

    try {
        app_.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        exit_code_ = app_.exit(e);
        return false;
    }
    exit_code_ = 0;

    // Fold the deprecated flag into the registry sink, then log the effective
    // overrides BEFORE any move leaves the vector empty. The logger is
    // initialized here (idempotent; RunApplication re-applies the level).
    overrides_.force_disabled_extensions.insert(overrides_.force_disabled_extensions.end(),
                                               force_disabled_extensions_.begin(),
                                               force_disabled_extensions_.end());
    VulkanEngine::Startup::InitializeLogger(log_level_);
    LOGIFACE_LOG(info, "effective overrides: " + overrides_.DescribeEffective());

    return true;
}

VulkanEngine::Application::ApplicationConfig Cli::MakeConfig() const {
    VulkanEngine::Application::ApplicationConfig config{};
    config.app_name = app_name_;
    config.log_level = log_level_;
    // The config's default is build-type aware (debug: on, optimized: off);
    // explicit CLI flags override it in either direction. Both given: --validation wins.
    config.bootstrap_config.enable_validation =
        force_validation_ ||
        (config.bootstrap_config.enable_validation && !force_no_validation_);
    config.bootstrap_config.force_disabled_extensions = overrides_.force_disabled_extensions;
    return config;
}

} // namespace Runtime