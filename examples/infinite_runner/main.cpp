#include <CLI/CLI.hpp>

#include "engine/core/EntryPoint.hpp"

import std;

import Runtime.Application;
import Runtime.ExampleCli;
import Examples.InfiniteRunner.Game;

int VulkanEngine::AppMain(int argc, char* const argv[]) {
    Runtime::Cli cli{"VulkanEngineV5 Infinite Runner"};

    if (!cli.Parse(argc, argv)) {
        return cli.ExitCode();
    }

    auto game = std::make_unique<Examples::InfiniteRunner::Game::Game>(cli.ExecutablePath());

    return VulkanEngine::Application::RunApplication(cli.MakeConfig(), game->GetHooks());
}
